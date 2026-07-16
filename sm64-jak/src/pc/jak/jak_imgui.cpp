/**
 * jak_imgui.cpp - Dear ImGui overlay for SM64-Jak.
 *
 * Toggled with Left Alt. Rendered right before the buffer swap (called from
 * gfx_pc.c after jak_sm64_render). Uses a minimal custom backend instead of
 * the stock imgui SDL2/GL2 backends: input is polled from SDL each frame
 * (mouse + Left Alt only — enough for menu interaction) and drawing is
 * fixed-function OpenGL 2, matching SM64's GL 2.1 context.
 */

#include "imgui/imgui.h"

#ifdef __MINGW32__
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern "C" {
/* From jakopengoal.c */
extern bool g_jak_world_view;          /* boot flag: gk renderer running */
extern bool g_jak_world_view_visible;  /* overlay flag: draw the PiP */
extern bool g_jak_display_swapped;     /* gk fullscreen + SM64 PiP (Tab) */
extern bool g_jak_debug_fly;           /* hold R2 to fly while enabled */
extern bool g_jak_shadow;              /* dynamic silhouette shadow */
void jak_settings_save(void);

struct JakWarpEntry {
    const char* name;
    int level;
};
const JakWarpEntry* jak_get_warp_table(int* count);
void jak_sm64_warp_to(int level);

int jak_controller_count(void);
const char* jak_controller_name(int idx);
bool jak_controller_is_gamepad(int idx);
int jak_controller_active(void);
void jak_controller_select(int idx);

void jak_imgui_frame(void);
}

static bool s_imgui_inited = false;
static bool s_visible = false;
static bool s_lalt_prev = false;
static GLuint s_font_tex = 0;
static bool s_mouse_prev[3] = {false, false, false};

static void jak_imgui_init(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;  /* no imgui.ini clutter next to the exe */
    io.BackendPlatformName = "jak_sm64_polled";
    io.BackendRendererName = "jak_sm64_gl2";
    ImGui::StyleColorsDark();

    /* Build + upload font atlas */
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(1, &s_font_tex);
    glBindTexture(GL_TEXTURE_2D, s_font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)s_font_tex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

    s_imgui_inited = true;
}

/* Fixed-function GL2 renderer (condensed imgui_impl_opengl2 equivalent) */
static void jak_imgui_render_draw_data(ImDrawData* dd) {
    int fb_w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
    int fb_h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    if (fb_w <= 0 || fb_h <= 0) return;

    GLint prev_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glUseProgram(0);

    /* SM64's renderer leaves VBOs bound — with GL_ARRAY_BUFFER bound, the
     * client-array pointers below would be treated as VBO offsets and
     * nothing would draw. Unbind, restore after. */
    GLint prev_array_buffer = 0, prev_element_buffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_element_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* Work on texture unit 0 — SM64's multitexture shaders may leave unit 1
     * active, and fixed-function sampling below assumes unit 0. */
    GLint prev_active_texture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);

    /* Stale generic vertex-attrib arrays (from SM64's shaders) can alias
     * attribute 0 over glVertexPointer on some drivers — disable, restore. */
    GLint attrib_was_enabled[16];
    for (int a = 0; a < 16; a++) {
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib_was_enabled[a]);
        if (attrib_was_enabled[a]) glDisableVertexAttribArray(a);
    }

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT | GL_TEXTURE_BIT |
                 GL_SCISSOR_BIT | GL_VIEWPORT_BIT | GL_DEPTH_BUFFER_BIT);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glViewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x,
            dd->DisplayPos.y + dd->DisplaySize.y, dd->DisplayPos.y, -1.0, +1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    ImVec2 clip_off = dd->DisplayPos;
    ImVec2 clip_scale = dd->FramebufferScale;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vtx = cl->VtxBuffer.Data;
        const ImDrawIdx* idx = cl->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                        (const void*)((const char*)vtx + offsetof(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          (const void*)((const char*)vtx + offsetof(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       (const void*)((const char*)vtx + offsetof(ImDrawVert, col)));
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd* cmd = &cl->CmdBuffer[c];
            if (cmd->UserCallback) {
                cmd->UserCallback(cl, cmd);
                continue;
            }
            ImVec2 cmin((cmd->ClipRect.x - clip_off.x) * clip_scale.x,
                        (cmd->ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 cmax((cmd->ClipRect.z - clip_off.x) * clip_scale.x,
                        (cmd->ClipRect.w - clip_off.y) * clip_scale.y);
            if (cmax.x <= cmin.x || cmax.y <= cmin.y) continue;
            glScissor((int)cmin.x, (int)((float)fb_h - cmax.y),
                      (int)(cmax.x - cmin.x), (int)(cmax.y - cmin.y));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->ElemCount,
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           idx + cmd->IdxOffset);
        }
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();

    for (int a = 0; a < 16; a++) {
        if (attrib_was_enabled[a]) glEnableVertexAttribArray(a);
    }
    glActiveTexture((GLenum)prev_active_texture);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prev_element_buffer);
    glUseProgram((GLuint)prev_program);
}

static void jak_imgui_menu(void) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Jak Integration (Left Alt)", &s_visible)) {
        if (g_jak_world_view) {
            ImGui::Checkbox("Show gk world PiP", &g_jak_world_view_visible);
            ImGui::Checkbox("Swap displays (Tab)", &g_jak_display_swapped);
            if (g_jak_display_swapped) {
                ImGui::TextDisabled("gk fullscreen: controller drives the Jak world");
            }
        } else {
            ImGui::BeginDisabled();
            bool off = false;
            ImGui::Checkbox("Show gk world PiP", &off);
            ImGui::EndDisabled();
            ImGui::TextDisabled("gk renderer disabled at boot\n(g_jak_world_view was false)");
        }
        if (ImGui::Checkbox("Jak shadow", &g_jak_shadow)) {
            jak_settings_save();  /* remember across launches */
        }
        ImGui::Checkbox("Debug fly (hold R2)", &g_jak_debug_fly);
        if (g_jak_debug_fly && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hold R2 to rise / hover, steer with the stick.\nRelease to fall.");
        }

        ImGui::Separator();

        /* Level warp dropdown */
        ImGui::TextUnformatted("Level warp");
        int warp_count = 0;
        const JakWarpEntry* warps = jak_get_warp_table(&warp_count);
        static int warp_sel = 0;
        if (warp_sel >= warp_count) warp_sel = 0;
        ImGui::SetNextItemWidth(-70.0f);
        /* HeightLargest: the polled-input backend has no mouse wheel, so show
         * as many entries as fit to minimize scrollbar dragging */
        if (ImGui::BeginCombo("##warp_level", warps[warp_sel].name, ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < warp_count; i++) {
                bool selected = (i == warp_sel);
                if (ImGui::Selectable(warps[i].name, selected)) warp_sel = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Warp")) {
            jak_sm64_warp_to(warps[warp_sel].level);
        }

        /* Controllers (collapsed by default) */
        if (ImGui::CollapsingHeader("Controllers")) {
            int count = jak_controller_count();
            int active = jak_controller_active();
            if (count == 0) {
                ImGui::TextDisabled("No controllers detected");
            }
            for (int i = 0; i < count; i++) {
                char label[280];
                bool gamepad = jak_controller_is_gamepad(i);
                snprintf(label, sizeof(label), "%d: %s%s", i, jak_controller_name(i),
                         gamepad ? "" : " (no gamepad mapping)");
                if (!gamepad) ImGui::BeginDisabled();
                if (ImGui::RadioButton(label, i == active) && i != active) {
                    jak_controller_select(i);
                }
                if (!gamepad) ImGui::EndDisabled();
            }
        }

        ImGui::Separator();
        ImGui::Text("%.1f FPS", io.Framerate);
    }
    ImGui::End();
}

void jak_imgui_frame(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    bool lalt = keys && keys[SDL_SCANCODE_LALT];
    if (lalt && !s_lalt_prev) s_visible = !s_visible;
    s_lalt_prev = lalt;

    if (!s_visible) return;

    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (!window) return;

    if (!s_imgui_inited) jak_imgui_init();

    ImGuiIO& io = ImGui::GetIO();

    /* Display size + framebuffer scale */
    int ww = 0, wh = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (ww <= 0 || wh <= 0) return;
    io.DisplaySize = ImVec2((float)ww, (float)wh);
    io.DisplayFramebufferScale = ImVec2((float)dw / (float)ww, (float)dh / (float)wh);

    /* Delta time */
    static Uint64 last_time = 0;
    Uint64 now = SDL_GetPerformanceCounter();
    io.DeltaTime = last_time
                       ? (float)((double)(now - last_time) / (double)SDL_GetPerformanceFrequency())
                       : (1.0f / 60.0f);
    if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
    last_time = now;

    /* Polled mouse input (window coordinates match DisplaySize) */
    int mx = 0, my = 0;
    Uint32 mstate = SDL_GetMouseState(&mx, &my);
    io.AddMousePosEvent((float)mx, (float)my);
    bool mdown[3] = {(mstate & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0,
                     (mstate & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0,
                     (mstate & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0};
    if (mdown[0] != s_mouse_prev[0]) io.AddMouseButtonEvent(ImGuiMouseButton_Left, mdown[0]);
    if (mdown[1] != s_mouse_prev[1]) io.AddMouseButtonEvent(ImGuiMouseButton_Right, mdown[1]);
    if (mdown[2] != s_mouse_prev[2]) io.AddMouseButtonEvent(ImGuiMouseButton_Middle, mdown[2]);
    s_mouse_prev[0] = mdown[0];
    s_mouse_prev[1] = mdown[1];
    s_mouse_prev[2] = mdown[2];

    ImGui::NewFrame();
    jak_imgui_menu();
    ImGui::Render();
    jak_imgui_render_draw_data(ImGui::GetDrawData());
}

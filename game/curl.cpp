#include <third-party/curl/include/curl/curl.h>
#include <iostream>
#include <string>

int test()
{
    // Initialize curl
    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();

    if (curl)
    {
        // Set the URL to request
        curl_easy_setopt(curl, CURLOPT_URL, "http://www.google.com");

        // Set the write function to handle the response
        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](char* data, size_t size, size_t nmemb, std::string* writerData) -> size_t {
            writerData->append(data, size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Perform the request
        CURLcode res = curl_easy_perform(curl);

        if (res == CURLE_OK)
        {
            // Output the response to the console window
            std::cout << response << std::endl;
        }
        else
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        // Clean up curl
        curl_easy_cleanup(curl);
    }

    // Clean up global curl state
    curl_global_cleanup();
    return 0;
}

void performCurlRequest()
{
    // Initialize curl
    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();

    if (curl)
    {
        // Set the URL to request
        curl_easy_setopt(curl, CURLOPT_URL, "http://www.google.com");

        // Set the write function to handle the response
        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](char* data, size_t size, size_t nmemb, std::string* writerData) -> size_t {
            writerData->append(data, size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Perform the request
        CURLcode res = curl_easy_perform(curl);

        if (res == CURLE_OK)
        {
            // Output the response to the console window
            std::cout << response << std::endl;
        }
        else
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        // Clean up curl
        curl_easy_cleanup(curl);
    }

    // Clean up global curl state
    curl_global_cleanup();
}

// Callback function for the ImGui button
void onButtonClicked()
{
    performCurlRequest();
}

// Example usage of the ImGui button
//if (ImGui::Button("Make Request"))
//{
//    onButtonClicked();
//}
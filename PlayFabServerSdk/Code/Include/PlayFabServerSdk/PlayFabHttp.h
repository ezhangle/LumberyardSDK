#pragma once

#include <PlayFabServerSdk/PlayFabError.h>

#include <aws/core/http/HttpTypes.h>
#include <aws/core/http/HttpResponse.h>
#include <AzCore/std/parallel/atomic.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/parallel/conditional_variable.h>
#include <AzCore/JSON/document.h>

namespace PlayFabServerSdk
{
    class PlayFabRequest
    {
    public:
        typedef std::function<void(PlayFabRequest* request)> HttpCallback;

        // Initializing ctor
        PlayFabRequest(const AZStd::string& URI, Aws::Http::HttpMethod method, const AZStd::string& authKey, const AZStd::string& authValue, const AZStd::string& requestJsonBody, void* customData, void* mResultCallback, ErrorCallback mErrorCallback, const HttpCallback& internalCallback);
        ~PlayFabRequest();

        // #THIRD_KIND_PLAYFAB_REQUEST_CALLBACK_LINUX: Allow conversion of ProcessApiCallback<T> to void*. Fixes no matching constructor for initialization of 'PlayFabServerSdk::PlayFabRequest'. no known conversion from 'ProcessApiCallback<XXX>' (aka 'void (*)(const XXX &, void *)') to 'void *'
        template < typename T >
        inline PlayFabRequest(const AZStd::string& URI, Aws::Http::HttpMethod method, const AZStd::string& authKey, const AZStd::string& authValue, const AZStd::string& requestJsonBody, void* customData, T mResultCallback, ErrorCallback mErrorCallback, const HttpCallback& internalCallback)
            : PlayFabRequest(URI, method, authKey, authValue, requestJsonBody, customData, (void*)mResultCallback, mErrorCallback, internalCallback)
        {
            static_assert(sizeof(T) <= sizeof(void*), "Size of function pointer is larger than void*, bad cast will occur.");
        }

        void HandleErrorReport(); // Call this when the response information describes an error (this parses that information into mError, and activates the error callback)

        // the URI in string form as an recipient of the HTTP connection
        AZStd::string mURI;
        // the method of which the HTTP request will take. GET, POST, DELETE, PUT, or HEAD
        Aws::Http::HttpMethod mMethod;
        // Authentication, when present
        AZStd::string mAuthKey;
        AZStd::string mAuthValue;
        // Json request body
        AZStd::string mRequestJsonBody;
        // Customizable object that provides identification or other information for the caller, in the callback
        void* mCustomData;

        // Everything about the response
        std::shared_ptr<Aws::Http::HttpResponse> httpResponse;
        char* mResponseText; // If the server responded, this will be the raw text returned
        int mResponseSize; // If the server responded, this will be the size of the response
        rapidjson::Document* mResponseJson; // If the server responded with json text, this will be a json Document describing the result from the server
        PlayFabError* mError; // If error, this will be a description of the error
        // This will always be the response code from the server
        Aws::Http::HttpResponseCode mHttpCode;
        // the function of which to feed back the JSON that the HTTP call resulted in. The function also requires the HTTPResponseCode indicating if the call was successful or failed
        // the signature of this callback will probably have to change
        HttpCallback mInternalCallback;
        void* mResultCallback;
        ErrorCallback mErrorCallback;
    private:
        // Disable copy constructor and assignment by making them private
        PlayFabRequest(const PlayFabRequest&);
        PlayFabRequest& operator=(const PlayFabRequest&);
    };

    class PlayFabRequestManager
    {
    public:
        static PlayFabRequestManager *playFabHttp;

        PlayFabRequestManager();
        ~PlayFabRequestManager();

        // see IHttpRequestManager::AddRequest
        // Add these parameters to a queue of request parameters to send off as an HTTP request as soon as they reach the head of the queue
        void AddRequest(PlayFabRequest* httpRequestParameters);
        int GetPendingCalls(); // Return the number of unfinished calls

    private:
        // PlayFabRequestManager thread loop. 
        void ThreadFunction();

        // Perform an HTTP request.  Not sure if this one blocks, but if it does, it's very short
        void HandleRequest(PlayFabRequest* httpRequestParameters);
        // For the request, block until a response is received, then give the returned JSON to the callback to parse.
        void HandleResponse(PlayFabRequest* requestContainer);

        // Collection of requests
        int m_pendingCalls;
        AZStd::queue<PlayFabRequest*> m_requestsToHandle;

        // Member variables for synchronization
        AZStd::mutex m_requestMutex;

        // Run flag used to signal the worker thread
        AZStd::atomic<bool> m_runThread;

        // This is the thread that will be used for all async operations
        AZStd::thread m_thread;
    };
}

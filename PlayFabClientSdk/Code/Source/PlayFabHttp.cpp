#include "StdAfx.h"
#include <PlayFabClientSdk/PlayFabHttp.h>
#include "PlayFabSettings.h"

#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/http/HttpClientFactory.h>
#include <AzCore/std/parallel/lock.h>

using namespace PlayFabClientSdk;
using namespace rapidjson;

// #THIRD_KIND_PLAYFAB_HTTP_DEBUGGING: Added debug logging and delay to http request manager
//#define PLAYFAB_DEBUG_HTTP_LOG              // Enable to log requests and responses to the tty
//#define PLAYFAB_DEBUG_DELAY_RESPONSE 5000   // Enable to introduce an artificial delay on responses (time in milliseconds)

///////////////////// PlayFabRequest /////////////////////
PlayFabRequest::PlayFabRequest(const AZStd::string& URI, Aws::Http::HttpMethod method, const AZStd::string& authKey, const AZStd::string& authValue, const AZStd::string& requestJsonBody, void* customData, void* mResultCallback, ErrorCallback mErrorCallback, const HttpCallback& internalCallback)
    : mURI(URI)
    , mMethod(method)
    , mAuthKey(authKey)
    , mAuthValue(authValue)
    , mRequestJsonBody(requestJsonBody)
    , mCustomData(customData)
    , mResponseText(nullptr)
    , mResponseSize(0)
    , mResponseJson(nullptr)
    , mError(nullptr)
    , mHttpCode(Aws::Http::HttpResponseCode::BAD_REQUEST)
    , mInternalCallback(internalCallback)
    , mResultCallback(mResultCallback)
    , mErrorCallback(mErrorCallback)
{
}

PlayFabRequest::~PlayFabRequest()
{
    if (mResponseText != nullptr)
        delete mResponseText;
    if (mError != nullptr)
        delete mError;
    if (mResponseJson != nullptr)
        delete mResponseJson;
}

void PlayFabRequest::HandleErrorReport()
{
    mError = new PlayFabError;

    if (mResponseSize != 0 // Not a null response
        && mResponseJson->GetParseError() == kParseErrorNone) // Proper json response
    {
        // If we have a proper json response, try to parse that json into our error-result format
        auto end = mResponseJson->MemberEnd();
        auto errorCodeJson = mResponseJson->FindMember("errorCode");
        mError->ErrorCode = (errorCodeJson != end && errorCodeJson->value.IsNumber()) ? static_cast<PlayFabErrorCode>(errorCodeJson->value.GetInt()) : PlayFabErrorServiceUnavailable;
        auto codeJson = mResponseJson->FindMember("code");
        mError->HttpCode = (codeJson != end && codeJson->value.IsNumber()) ? codeJson->value.GetInt() : 503;
        auto statusJson = mResponseJson->FindMember("status");
        mError->HttpStatus = (statusJson != end && statusJson->value.IsString()) ? statusJson->value.GetString() : "ServiceUnavailable";
        auto errorNameJson = mResponseJson->FindMember("error");
        mError->ErrorName = (errorNameJson != end && errorNameJson->value.IsString()) ? errorNameJson->value.GetString() : "ServiceUnavailable";
        auto errorMessageJson = mResponseJson->FindMember("errorMessage");
        mError->ErrorMessage = (errorMessageJson != end && errorMessageJson->value.IsString()) ? errorMessageJson->value.GetString() : mResponseText;
        auto errorDetailsJson = mResponseJson->FindMember("errorDetails");
        if (errorDetailsJson != end && errorDetailsJson->value.IsObject())
        {
            const Value& errorDetailsObj = errorDetailsJson->value;
            for (Value::ConstMemberIterator itr = errorDetailsObj.MemberBegin(); itr != errorDetailsObj.MemberEnd(); ++itr)
            {
                if (itr->name.IsString() && itr->value.IsArray())
                {
                    const Value& errorList = itr->value;
                    for (Value::ConstValueIterator erroListIter = errorList.Begin(); erroListIter != errorList.End(); ++erroListIter)
                        mError->ErrorDetails.insert(std::pair<AZStd::string, AZStd::string>(itr->name.GetString(), erroListIter->GetString()));
                }
            }
        }
    }
    else
    {
        // If we get here, we failed to connect meaningfully to the server - either a timeout, or a non-json response (which means aws failed or something)
        mError->HttpCode = mResponseSize == 0 ? 408 : 503; // 408 for no response, 503 for a non-json response
        mError->HttpStatus = mResponseSize == 0 ? "RequestTimeout" : "ServiceUnavailable";
        mError->ErrorCode = mResponseSize == 0 ? PlayFabErrorConnectionTimeout : PlayFabErrorServiceUnavailable;
        mError->ErrorName = mResponseSize == 0 ? "ConnectionTimeout" : "ServiceUnavailable";
        // For text returns, use the non-json response if possible, else default to no response
        mError->ErrorMessage = mError->HttpStatus = mResponseSize == 0 ? "Request Timeout or null response" : mResponseText;
    }

    // Send the error callbacks
    if (PlayFabSettings::playFabSettings->globalErrorHandler != nullptr)
        PlayFabSettings::playFabSettings->globalErrorHandler(*mError, mCustomData);
    if (mErrorCallback != nullptr)
        mErrorCallback(*mError, mCustomData);
}

///////////////////// PlayFabRequestManager /////////////////////
PlayFabRequestManager * PlayFabRequestManager::playFabHttp = nullptr;

PlayFabRequestManager::PlayFabRequestManager()
{
    m_runThread = true;
    auto function = std::bind(&PlayFabRequestManager::ThreadFunction, this);
    m_thread = AZStd::thread(function);
}

PlayFabRequestManager::~PlayFabRequestManager()
{
    m_runThread = false;
    if (m_thread.joinable())
        m_thread.join();
}

int PlayFabRequestManager::GetPendingCalls()
{
    int temp;
    {
        AZStd::lock_guard<AZStd::mutex> lock(m_requestMutex);
        temp = m_pendingCalls;
    }
    return temp;
}

void PlayFabRequestManager::AddRequest(PlayFabRequest* requestContainer)
{
    {
        AZStd::lock_guard<AZStd::mutex> lock(m_requestMutex);
        m_requestsToHandle.push(AZStd::move(requestContainer));
    }
}

void PlayFabRequestManager::ThreadFunction()
{
    AZStd::queue<PlayFabRequest*> requestsToHandle, resultsToHandle;

    // Run the thread as long as directed
    while (m_runThread)
    {
        {
            AZStd::lock_guard<AZStd::mutex> lock(m_requestMutex);
            requestsToHandle.swap(m_requestsToHandle);
            m_pendingCalls = requestsToHandle.size() + resultsToHandle.size();
        }

        // Send all the requests immediately to make API calls work in Parellel, these don't really wait (much if at all <1 ms)
        while (!requestsToHandle.empty())
        {
            auto request = requestsToHandle.front();
            HandleRequest(request);
            resultsToHandle.push(request);
            requestsToHandle.pop();
        }

        // Handle a single result this tick (blocking call takes 50-500 ms) - Results in Serial unfortunately, can't evaluate which has returned yet (maybe something to fix)
        if (!resultsToHandle.empty())
        {
            auto request = resultsToHandle.front();
            HandleResponse(request);
            resultsToHandle.pop();
        }
        else
            CrySleep(33); // Don't thrash this thread too hard when there's no requests active
    }
}

void PlayFabRequestManager::HandleRequest(PlayFabRequest* requestContainer)
{
    std::shared_ptr<Aws::Http::HttpClient> httpClient = Aws::Http::CreateHttpClient(Aws::Client::ClientConfiguration());

    auto httpRequest = Aws::Http::CreateHttpRequest(Aws::String(requestContainer->mURI.c_str()), requestContainer->mMethod, Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
    // #THIRD_KIND_PLAYFAB_HTTP_DEBUGGING: Added debug logging and delay to http request manager
#if defined (PLAYFAB_DEBUG_HTTP_LOG)
    AZ_TracePrintf("PlayFab", "*** PlayFab Request - %s %s", Aws::Http::HttpMethodMapper::GetNameForHttpMethod(requestContainer->mMethod), requestContainer->mURI.c_str());
#endif

    httpRequest->SetContentType("application/json");
    httpRequest->SetHeaderValue("X-PlayFabSDK", Aws::String(PlayFabSettings::playFabSettings->playFabVersionString.c_str()));
    if (requestContainer->mAuthKey.length() > 0)
        httpRequest->SetHeaderValue(Aws::String(requestContainer->mAuthKey.c_str()), Aws::String(requestContainer->mAuthValue.c_str()));

    auto sharedStream(Aws::MakeShared<Aws::StringStream>("PlayFabHttp AZStd::stringStream"));
    *sharedStream << requestContainer->mRequestJsonBody.c_str();
    httpRequest->AddContentBody(sharedStream);
    httpRequest->SetContentLength(std::to_string(requestContainer->mRequestJsonBody.length()).c_str());
    requestContainer->httpResponse = httpClient->MakeRequest(*httpRequest);
}

void PlayFabRequestManager::HandleResponse(PlayFabRequest* requestContainer)
{
    if (!requestContainer || !requestContainer->httpResponse)
        return;

    // #THIRD_KIND_PLAYFAB_HTTP_DEBUGGING: Added debug logging and delay to http request manager
#if defined (PLAYFAB_DEBUG_DELAY_RESPONSE)
    CrySleep(PLAYFAB_DEBUG_DELAY_RESPONSE);
#endif

    requestContainer->mHttpCode = requestContainer->httpResponse->GetResponseCode();
    Aws::IOStream& responseStream = requestContainer->httpResponse->GetResponseBody();
    responseStream.seekg(0, std::ios_base::end);
    requestContainer->mResponseSize = responseStream.tellg();
    responseStream.seekg(0, std::ios_base::beg);
    requestContainer->mResponseText = new char[requestContainer->mResponseSize + 1];
    responseStream.read(requestContainer->mResponseText, requestContainer->mResponseSize);
    requestContainer->mResponseText[requestContainer->mResponseSize] = '\0';
    requestContainer->mResponseJson = new rapidjson::Document;
    requestContainer->mResponseJson->Parse<0>(requestContainer->mResponseText);
#if defined (PLAYFAB_DEBUG_HTTP_LOG)
    AZ_TracePrintf("PlayFab", "*** PlayFab Response - %s %s, Response: %s", Aws::Http::HttpMethodMapper::GetNameForHttpMethod(requestContainer->mMethod), requestContainer->mURI.c_str(), requestContainer->mResponseText);
#endif
    requestContainer->mInternalCallback(requestContainer);
}

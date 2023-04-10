//------------------------------------------------------------------------------
//  curlhttpclient.cc
//  (C) 2012 Bigpoint GmbH
//------------------------------------------------------------------------------
#include "stdneb.h"
#include "core/config.h"
#if __NEBULA3_CURL_HTTPCLIENT__
#include "curlhttpclient.h"
#include "threading/contextlock.h"
#include "threading/thread.h"
#include "http/httprequest.h"
#include <string>

#if __WIN32__
// under Windows, make sure to use the self-compiled CURL
#define CURL_STATICLIB (1)
#include "curl-7.24.0/include/curl/curl.h"
#else
// under Linux, use the system curl .so
#include <curl/curl.h>
#endif

namespace Http
{
__ImplementClass(Http::CurlHttpClient, 'CHTC', Core::RefCounted);

bool CurlHttpClient::curlInitCalled = false;
Threading::CriticalSection CurlHttpClient::curlInitCriticalSection;

using namespace Util;
using namespace IO;

//------------------------------------------------------------------------------
/**
    Curl alloc memory callback.
*/
void*
CurlHttpClient::CurlMalloc(size_t size)
{
    return N3_ALLOC(Memory::NetworkHeap, size);
}

//------------------------------------------------------------------------------
/**
    Curl free memory callback.
*/
void
CurlHttpClient::CurlFree(void* ptr)
{
    N3_FREE(Memory::NetworkHeap, ptr);
}

//------------------------------------------------------------------------------
/**
    Curl realloc memory callback.
*/
void*
CurlHttpClient::CurlRealloc(void* ptr, size_t size)
{
    return Memory::Realloc(Memory::NetworkHeap, ptr, size);
}

//------------------------------------------------------------------------------
/**
    Curl strdup memory callback.
*/
char*
CurlHttpClient::CurlStrdup(const char* str)
{
    n_assert(0 != str);
    SizeT numBytes = String::StrLen(str) + 1;
    char* buf = (char*) N3_ALLOC(Memory::NetworkHeap, numBytes);
    Memory::Copy(str, buf, numBytes);
    return buf;
}

//------------------------------------------------------------------------------
/**
    Curl calloc memory callback.
*/
void*
CurlHttpClient::CurlCalloc(size_t nmemb, size_t size)
{
    SizeT numBytes = SizeT(nmemb * size);
    void* buf = N3_ALLOC(Memory::NetworkHeap, numBytes);
    Memory::Clear(buf, numBytes);
    return buf;
}

//------------------------------------------------------------------------------
/**
    Curl write data memory callback. User data is expected to be a
    pointer to a Nebula3 Stream object, opened for writing.
*/
size_t
CurlHttpClient::CurlWriteData(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t numBytes = size * nmemb;
    if (numBytes > 0)
    {
        Stream* stream = (Stream*) userdata;
        n_assert(stream->IsA(Stream::RTTI));
        stream->Write(ptr, Stream::Size(numBytes));
        return numBytes;
    }
    else
    {
        return 0;
    }
}

//------------------------------------------------------------------------------
/**
*/
CurlHttpClient::CurlHttpClient() :
    fillResponseContentStreamOnError(false),
    cancelOnThreadStopRequested(true),
    recvTimeout(0),
    curlHandle(0),
    lastRequestTime(0),
    redirectResponseCount(0)
{
    // check if we must setup curl, this must be called once for the 
    // whole program, and since curl_global_init() is not thread-safe
    // we must protect from multiple execution
    Threading::ContextLock lock(this->curlInitCriticalSection);
    if (!this->curlInitCalled)
    {
        CURLcode res = curl_global_init_mem(CURL_GLOBAL_ALL, CurlMalloc, CurlFree, CurlRealloc, CurlStrdup, CurlCalloc);
        n_assert2(0 == res, "CurlHttpClient: curl_global_init() failed!\n");
        this->curlInitCalled = true;
    }
    const SizeT curlErrorBufSize = CURL_ERROR_SIZE * 4;
    this->curlError = (char*) N3_ALLOC(Memory::ScratchHeap, curlErrorBufSize);
    Memory::Clear(this->curlError, curlErrorBufSize);
}

//------------------------------------------------------------------------------
/**
*/
CurlHttpClient::~CurlHttpClient()
{
    if (this->IsConnected())
    {
        this->Disconnect();
    }
    N3_FREE(Memory::ScratchHeap, this->curlError);
    this->curlError = 0;
}

//------------------------------------------------------------------------------
/**
*/
bool
CurlHttpClient::Connect(const URI& uri)
{
    n_assert(!this->IsConnected());

    // store the connection url
    this->serverUri = uri;
    this->effectiveServerUrl = uri;

    // get a new curl session, ideally there's one curl session per
    // thread, the HttpClientRegistry takes care of this since it hands out 
    // shared HttpClient objects
    this->curlHandle = curl_easy_init();
    n_assert2(0 != this->curlHandle, "CurlHttpClient: curl_easy_init() failed!\n");
    
    // set some general options for this curl handle
    // NOTE: better don't mess with CURL timeouts, there are quite a 
    // lot of clients which take quite a long time for name resolution (for instance)
    curl_easy_setopt(this->curlHandle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(this->curlHandle, CURLOPT_NOPROGRESS, 1L);

    curl_easy_setopt(this->curlHandle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(this->curlHandle, CURLOPT_COOKIEFILE, "");

    // only support http protocol
    //curl_easy_setopt(this->curlHandle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);

    curl_easy_setopt(this->curlHandle, CURLOPT_ERRORBUFFER, this->curlError);
    curl_easy_setopt(this->curlHandle, CURLOPT_WRITEFUNCTION, CurlWriteData);
    curl_easy_setopt(this->curlHandle, CURLOPT_USERAGENT, "Mozilla");
    /*
    HMM THIS WOULD MAKE SENSE, BUT IS ONLY AVAILABLE SINCE CURL 7.25:
    curl_easy_setopt(this->curlHandle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(this->curlHandle, CURLOPT_TCP_KEEPIDLE, 10L);
    curl_easy_setopt(this->curlHandle, CURLOPT_TCP_KEEPINTVL, 10L);
    */

    curl_easy_setopt(this->curlHandle, CURLOPT_URL, uri.AsString().AsCharPtr());
    curl_easy_setopt(this->curlHandle, CURLOPT_SSL_VERIFYPEER, false);
    curl_easy_setopt(this->curlHandle, CURLOPT_SSL_VERIFYHOST, false);

    long curlTimeout = (long) this->recvTimeout;
    if (curlTimeout > 0)
    {
        // this basically checks whether the connection has been interrupted
        curl_easy_setopt(this->curlHandle, CURLOPT_LOW_SPEED_LIMIT, 50);
        curl_easy_setopt(this->curlHandle, CURLOPT_LOW_SPEED_TIME, curlTimeout);
    }
    
    // setup idle timer stuff
    if (!this->idleTimer.Running())
    {
        this->idleTimer.Start();
    }
    this->lastRequestTime = this->idleTimer.GetTime();
    return true;
}

//------------------------------------------------------------------------------
/**
*/
void
CurlHttpClient::Disconnect()
{
    if (this->idleTimer.Running())
    {
        this->idleTimer.Stop();
    }
    if (this->IsConnected())
    {
        curl_easy_cleanup(this->curlHandle);
        this->curlHandle = 0;
    }
}

//------------------------------------------------------------------------------
/**
*/
bool
CurlHttpClient::IsConnected() const
{
    return (0 != this->curlHandle);
}

//------------------------------------------------------------------------------
/**
*/
Timing::Time
CurlHttpClient::GetIdleTime() const
{
    return this->idleTimer.GetTime() - this->lastRequestTime;
}

//------------------------------------------------------------------------------
/**
*/
HttpStatus::Code
CurlHttpClient::SendRequest(HttpMethod::Code requestMethod, const URI& uri, const Ptr<Stream>& responseContentStream)
{
    Ptr<HttpRequestWriter> httpRequestWriter = HttpRequestWriter::Create();
    httpRequestWriter->SetMethod(requestMethod);
    httpRequestWriter->SetURI(uri);
    HttpStatus::Code httpStatus = this->SendRequest(httpRequestWriter, responseContentStream);
    this->lastRequestTime = this->idleTimer.GetTime();
    return httpStatus;
}

//------------------------------------------------------------------------------
/**
*/
HttpStatus::Code
CurlHttpClient::SendRequest(const Ptr<HttpRequest>& request)
{
    HttpStatus::Code status = this->SendRequest(request->CreateRequestWriter(), request->GetResponseContentStream());
    request->SetEffectiveUri(this->effectiveServerUrl);
    return status;
}


//------------------------------------------------------------------------------
/**
*/
HttpStatus::Code
CurlHttpClient::SendRequest(const Ptr<HttpRequestWriter>& requestWriter, const Ptr<Stream>& responseContentStream, SizeT maxRetries)
{
    IndexT curRetry = 0;
    Timing::Time retrySleepDuration = __NEBULA3_HTTP_FILESYSTEM_INNER_RETRY_COOLDOWN__;
    HttpStatus::Code httpStatus = this->InternalSendRequest(requestWriter, responseContentStream);

    // retry if the request has failed with "common errors"
    while (((httpStatus == HttpStatus::ServiceUnavailable) || (httpStatus == HttpStatus::BadGateway) || (httpStatus == HttpStatus::Nebula3CurlEasyPerformFailed)) && (curRetry < maxRetries))
    {
        n_sleep(retrySleepDuration);
        curRetry++;
        n_warning("CurlHttpClient::SendRequest(): request '%s' failed with '%s', retry %d of %d...\n",
            requestWriter->GetURI().AsString().AsCharPtr(),
            HttpStatus::ToHumanReadableString(httpStatus).AsCharPtr(),
            curRetry, maxRetries);

        if (this->cancelOnThreadStopRequested)
        {
            // check if the reason for the failure was that our thread was requested to stop,
            // in this case we just return with an error
            if (Threading::Thread::GetMyThreadStopRequested())
            {
                // just "abuse" a NotFound http status
                n_warning("CurlHttpClient::SendRequest(): thread was requested to stop!\n");
                return HttpStatus::NotFound;
            }
        }

        // discard any partially received data and try again...
        responseContentStream->SetSize(0);
        httpStatus = this->InternalSendRequest(requestWriter, responseContentStream);
    }

    this->lastRequestTime = this->idleTimer.GetTime();
    return httpStatus;
}

//------------------------------------------------------------------------------
/**
*/
int 
CurlHttpClient::CurlDebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr)
{
    String s = data;
    n_dbgout("CurlHttpClient::CurlDebugCallback(): %s\n", s.AsCharPtr());
    return 0;
}

//------------------------------------------------------------------------------
/**
*/
String 
CurlHttpClient::modifyUrlToHttps(const std::string& httpUrlString) {
    std::string httpsUrlString = httpUrlString;
    size_t pos = httpsUrlString.find("http://");
    if (pos != std::string::npos) {
        httpsUrlString.replace(pos, 7, "https://");
    }
    return String(httpsUrlString.c_str());
    }
    
HttpStatus::Code
CurlHttpClient::InternalSendRequest(const Ptr<HttpRequestWriter>& requestWriter, const Ptr<Stream>& responseContentStream)
{
    HttpStatus::Code httpStatus = HttpStatus::OK;

    // first make sure we're connected (this actually cannot fail in the CurlHttpClient implementation
    if (!this->IsConnected())
    {
        bool connectResult = this->Connect(requestWriter->GetURI());
        n_assert(connectResult);
    }

    // set URL in curl
    String httpUrlString = requestWriter->GetURI().AsString();

    #if __NEBULA3_HTTP_FILESYSTEM_CURL_VERBOSE_MODE__
    data d;
    d.trace_ascii = 1;
    curl_easy_setopt(this->curlHandle, CURLOPT_DEBUGFUNCTION, CurlHttpClient::CurlDebugCallback);
    curl_easy_setopt(this->curlHandle, CURLOPT_DEBUGDATA, &d);
    curl_easy_setopt(this->curlHandle, CURLOPT_VERBOSE, 1);
    #endif
    String httpsUrlString = modifyUrlToHttps(httpUrlString.AsCharPtr());
    curl_easy_setopt(this->curlHandle, CURLOPT_URL, httpsUrlString.AsCharPtr());



    // set HTTP method in curl
    switch (requestWriter->GetMethod())
    {
        case HttpMethod::Get:
            curl_easy_setopt(this->curlHandle, CURLOPT_HTTPGET, 1);
            break;
        case HttpMethod::Post:
            curl_easy_setopt(this->curlHandle, CURLOPT_POST, 1);
            break;
        case HttpMethod::Put:
            curl_easy_setopt(this->curlHandle, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        default:
            n_error("CurlHttpClient::InternalSendRequest(): unsupported http method!\n");
            break;
    }

    // setup the HTTP header fields
    String maxAgeHeader;
    String contentTypeHeader;
    String contentLengthHeader = "Content-Length: 0"; // initialize to a valid HTTP protocol value
    if (requestWriter->GetCacheControlMaxAge() > 0)
    {
        maxAgeHeader.Format("Cache-Control: max-age=%d", requestWriter->GetCacheControlMaxAge());
    }
    const Ptr<Stream>& requestContentStream = requestWriter->GetContentStream();
    if (requestContentStream.isvalid())
    {
        if (requestContentStream->GetMediaType().IsValid())
        {
            contentTypeHeader.Format("Content-Type: %s", requestContentStream->GetMediaType().AsString().AsCharPtr());
        }
        contentLengthHeader.Format("Content-Length: %d", requestContentStream->GetSize());
    }
    struct curl_slist* headers = 0;
    if (maxAgeHeader.IsValid())
    {
        headers = curl_slist_append(headers, maxAgeHeader.AsCharPtr());
    }
    if (contentTypeHeader.IsValid())
    {
        headers = curl_slist_append(headers, contentTypeHeader.AsCharPtr());
    }
    if (contentLengthHeader.IsValid())
    {
        headers = curl_slist_append(headers, contentLengthHeader.AsCharPtr());
    }
    String xAuthHeader;
    if (requestWriter->GetXAuthToken().IsValid())
    {
        xAuthHeader.Format("X-Auth-Token: %s", requestWriter->GetXAuthToken().AsCharPtr());
        headers = curl_slist_append(headers, xAuthHeader.AsCharPtr());
    }
    headers = curl_slist_append(headers, "Connection: keep-alive");
    headers = curl_slist_append(headers, "Keep-Alive: 300");

    n_assert(0 != headers);
    curl_easy_setopt(this->curlHandle, CURLOPT_HTTPHEADER, headers);

    // if POST is used, set the data to post
    void* postData = 0;
    SizeT postDataSize = 0;
    if (HttpMethod::Post == requestWriter->GetMethod() || HttpMethod::Put == requestWriter->GetMethod() )
    {
        if (requestContentStream.isvalid())
        {
            requestContentStream->SetAccessMode(Stream::ReadAccess);
            if (requestContentStream->Open())
            {
                postDataSize = requestContentStream->GetSize();
                postData = requestContentStream->Map();
            }
        }
        if (0 != postData)
        {
            curl_easy_setopt(this->curlHandle, CURLOPT_POSTFIELDS, postData);
            curl_easy_setopt(this->curlHandle, CURLOPT_POSTFIELDSIZE, postDataSize);
        }
        else {
            // see: http://curl.haxx.se/libcurl/c/CURLOPT_POSTFIELDS.html
            curl_easy_setopt(this->curlHandle, CURLOPT_POSTFIELDSIZE, 0);
            curl_easy_setopt(this->curlHandle, CURLOPT_POSTFIELDS, 0L);
        }
    }

    // take care of the received data...
    responseContentStream->SetAccessMode(Stream::WriteAccess);
    if (!responseContentStream->Open())
    {
        n_error("CurlHttpClient::InternalSendRequest(): failed to open responseContentStream!\n");
    }
    curl_easy_setopt(this->curlHandle, CURLOPT_WRITEDATA, responseContentStream.get());

    // finally, perform the HTTP request and get the HTTP status code back
    
    CURLcode performResult = curl_easy_perform(this->curlHandle);
    long curlHttpCode = 0;
    curl_easy_getinfo(this->curlHandle, CURLINFO_RESPONSE_CODE, &curlHttpCode);
    httpStatus = (HttpStatus::Code) curlHttpCode;
    if (CURLE_PARTIAL_FILE == performResult)
    {
        // NOTE: This is the most prominent download error in the wild, and means that CURL
        // didn't receive the final chunk of a chunked file transform. We will treat this
        // as a warning for now. If the download is corrupted, then the MD5 check will complain later on.
        n_warning("CurlHttpRequest::InternalSendRequest(%s): curl_easy_perform() returned with CURLE_PARTIAL_FILE httpCode='%ld'\n",
            requestWriter->GetURI().AsString().AsCharPtr(), curlHttpCode);
    }
    else if (0 != performResult)
    {
        n_warning("CurlHttpRequest::InternalSendRequest(%s): curl_easy_perform() failed with '%s', httpCode='%ld'\n", 
            requestWriter->GetURI().AsString().AsCharPtr(), this->curlError, curlHttpCode);
        
        // hmm looks like CURL returns HTTP OK even if the connection went down halfway through the download
        // if this happens, replace the http code with Nebula3CurlEasyPerformFailed
        if ((HttpStatus::OK == curlHttpCode) || (0 == curlHttpCode))
        {
            httpStatus = HttpStatus::Nebula3CurlEasyPerformFailed;
        }
    }

    // get effective url for redirects
    char *effectiveUrl;
    CURLcode effectiveUrlResult = curl_easy_getinfo(this->curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    if (CURLE_OK == effectiveUrlResult && effectiveUrl)
    {
        this->effectiveServerUrl = IO::URI(effectiveUrl);
    }
    // get redirect count
    long redirectCount = 0;
    CURLcode redirectCountResult = curl_easy_getinfo(this->curlHandle, CURLINFO_REDIRECT_COUNT, &redirectCount);
    if (CURLE_OK == redirectCountResult)
    {
        this->redirectResponseCount = redirectCount;
    }

    // perform cleanup
    curl_easy_setopt(this->curlHandle, CURLOPT_WRITEDATA, 0);
    if (responseContentStream->IsOpen())
    {
        responseContentStream->Close();
    }
    if (0 != postData)
    {
        n_assert(requestContentStream.isvalid());
        requestContentStream->Unmap();
        requestContentStream->Close();
    }
    if (0 != headers)
    {
        curl_slist_free_all(headers);
        headers = 0;
    }

    return httpStatus;
}

//------------------------------------------------------------------------------
/**
*/
String
CurlHttpClient::GetErrorDesc() const
{
    return String(this->curlError);
}

} // namespace Http
#endif
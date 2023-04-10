#pragma once
#if __NEBULA3_CURL_HTTPCLIENT__
//------------------------------------------------------------------------------
/**
    @class Http::CurlHttpClient

    An all-bells'n'whistles HTTP client based on libcurl.

    (C) 2012 Bigpoint GmbH
*/
#include "core/config.h"
#include "core/refcounted.h"
#include "timing/time.h"
#include "http/httpstatus.h"
#include "http/httpmethod.h"
#include "http/httprequestwriter.h"
#include "io/uri.h"
#include "timing/timer.h"
#include <string>
#if __WIN32__
// under Windows, make sure to use the self-compiled CURL
#define CURL_STATICLIB (1)
#include "curl-7.24.0/include/curl/curl.h"
#else
// under Linux, use the system curl .so
#include <curl/curl.h>
#endif

//------------------------------------------------------------------------------
namespace Http
{
class HttpRequest;

class CurlHttpClient : public Core::RefCounted
{
    __DeclareClass(CurlHttpClient);
public:
    /// constructor
    CurlHttpClient();
    /// destructor
    virtual ~CurlHttpClient();

    /// set top true if response content stream should be filled even if HttpStatus is not OK (default is to ignore response content on error)
    void SetFillResponseContentStreamOnError(bool b);
    /// get the fill-response-content-stream-on-error flag (default is false)
    bool GetFillResponseContentStreamOnError() const;
    /// set to true if a lengthy download should be cancelled when the stop-requested flag is set (default is false)
    void SetCancelOnThreadStopRequested(bool b);
    /// get cancel-on-thread-stop requested flag
    bool GetCancelOnThreadStopRequested() const;
    /// set optional receive timeout in seconds
    void SetRecvTimeout(int secs);
    /// get optional receive timeout in seconds
    int GetRecvTimeout() const;

    /// establish a connection to a HTTP server
    virtual bool Connect(const IO::URI& uri);
    /// disconnect from the server
    virtual void Disconnect();
    /// return true if currently connected
    bool IsConnected() const;
    /// return the number of seconds since the last request was handled
    Timing::Time GetIdleTime() const;

    /// send request and write result to provided response content stream
    HttpStatus::Code SendRequest(HttpMethod::Code requestMethod, const IO::URI& uri, const Ptr<IO::Stream>& responseContentStream);
    /// send a request with a HttpRequest message object. Creates the HttpRequestWriter object (can also be used for PUT and POST) 
    /// and writes response into response content stream. Optionally sets effective URL on http redirect (301,302)
    HttpStatus::Code SendRequest(const Ptr<HttpRequest>& request);
    /// send a request with a completely configured HttpRequestWriter object (can also be used for PUT and POST)
    HttpStatus::Code SendRequest(const Ptr<HttpRequestWriter>& requestWriter, const Ptr<IO::Stream>& responseContentStream, SizeT maxRetries = __NEBULA3_HTTP_FILESYSTEM_MAX_RETRIES__);
    /// get extended error information (if the last request failed)
    Util::String GetErrorDesc() const;
    /// get effective server url
    const IO::URI& GetEffectiveUrl() const;
    /// get number of redirects
    long GetRedirectCount() const;
    /// change http to https 
    Util::String modifyUrlToHttps(const std::string& httpUrlString);

protected:
    /// malloc callback for curl
    static void* CurlMalloc(size_t size);
    /// free callback for curl
    static void CurlFree(void* ptr);
    /// realloc callback for curl
    static void* CurlRealloc(void* ptr, size_t size);
    /// strdup callback for curl
    static char* CurlStrdup(const char* str);
    /// calloc callback for curl
    static void* CurlCalloc(size_t nmemb, size_t size);
    /// data write callback for curl
    static size_t CurlWriteData(char* ptr, size_t size, size_t nmemb, void* userdata);
    /// internal send request method
    HttpStatus::Code InternalSendRequest(const Ptr<HttpRequestWriter>& requestWriter, const Ptr<IO::Stream>& responseContentStream);

    /// used by cURL verbose mode
    struct data {
        char trace_ascii; /* 1 or 0 */
    };
    /// callback method for curl verbose mode
    static int CurlDebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr);

    static bool curlInitCalled;
    static Threading::CriticalSection curlInitCriticalSection;
    bool fillResponseContentStreamOnError;
    bool cancelOnThreadStopRequested;
    IO::URI serverUri;
    IO::URI effectiveServerUrl;
    int recvTimeout;
    void* curlHandle;
    char* curlError;
    Timing::Timer idleTimer;
    Timing::Time lastRequestTime;
    long redirectResponseCount;
}; 

//------------------------------------------------------------------------------
/**
*/
inline void
CurlHttpClient::SetCancelOnThreadStopRequested(bool b)
{
    this->cancelOnThreadStopRequested = b;
}

//------------------------------------------------------------------------------
/**
*/
inline bool
CurlHttpClient::GetCancelOnThreadStopRequested() const
{
    return this->cancelOnThreadStopRequested;
}

//------------------------------------------------------------------------------
/**
*/
inline void
CurlHttpClient::SetFillResponseContentStreamOnError(bool b)
{
    this->fillResponseContentStreamOnError = b;
}

//------------------------------------------------------------------------------
/**
*/
inline bool
CurlHttpClient::GetFillResponseContentStreamOnError() const
{
    return this->fillResponseContentStreamOnError;
}

//------------------------------------------------------------------------------
/**
*/
inline void
CurlHttpClient::SetRecvTimeout(int secs)
{
    this->recvTimeout = secs;
}

//------------------------------------------------------------------------------
/**
*/
inline int
CurlHttpClient::GetRecvTimeout() const
{
    return this->recvTimeout;
}

//------------------------------------------------------------------------------
/**
*/
inline const IO::URI&
CurlHttpClient::GetEffectiveUrl() const
{
    return this->effectiveServerUrl;
}

//------------------------------------------------------------------------------
/**
*/
inline long
CurlHttpClient::GetRedirectCount() const
{
    return this->redirectResponseCount;
}

} // namespace Http
//------------------------------------------------------------------------------
#endif // __NEBULA3_CURL_HTTPCLIENT__
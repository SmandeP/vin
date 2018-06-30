// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2018 The Nodex developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcprotocol.h"

#include "clientversion.h"
#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"
#include "utiltime.h"
#include "version.h"

#include <stdint.h>

#include "json/json_spirit_writer_template.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/shared_ptr.hpp>

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace json_spirit;

//! Number of bytes to allocate and read at most at once in post data
const size_t POST_READ_SIZE = 256 * 1024;

/**
 * HTTP protocol
 * 
 * This ain't Apache.  We're just using HTTP header for the length field
 * and to be compatible with other JSON-RPC implementations.
 */

string HTTPPost(const string& strMsg, const map<string, string>& mapRequestHeaders)
{
    ostringstream s;
    s << "POST / HTTP/1.1\n"
      << "User-Agent: nodex-json-rpc/" << FormatFullVersion() << "\n"
      << "Host: 127.0.0.1\n"
      << "Content-Type: application/json\n"
      << "Content-Length: " << strMsg.size() << "\n"
      << "Connection: close\n"
      << "Accept: application/json\n";
    BOOST_FOREACH (const PAIRTYPE(string, string) & item, mapRequestHeaders)
        s << item.first << ": " << item.second << "\n";
    s << "\n"
      << strMsg;

    return s.str();
}

static string rfc1123Time()
{
    return DateTimeStrFormat("%a, %d %b %Y %H:%M:%S +0000", GetTime());
}

static const char* httpStatusDescription(int nStatus)
{
    switch (nStatus) {
    case HTTP_OK:
        return "OK";
    case HTTP_BAD_REQUEST:
        return "Bad Request";
    case HTTP_FORBIDDEN:
        return "Forbidden";
    case HTTP_NOT_FOUND:
        return "Not Found";
    case HTTP_INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    default:
        return "";
    }
}

string HTTPError(int nStatus, bool keepalive, bool headersOnly)
{
    if (nStatus == HTTP_UNAUTHORIZED)
        return strprintf("HTTP/1.0 401 Authorization Required\n"
                         "Date: %s\n"
                         "Server: nodex-json-rpc/%s\n"
                         "WWW-Authenticate: Basic realm=\"jsonrpc\"\n"
                         "Content-Type: text/html\n"
                         "Content-Length: 296\n"
                         "\n"
                         "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\n"
                         "\"http://www.w3.org/TR/1999/REC-html401-19991224/loose.dtd\">\n"
                         "<HTML>\n"
                         "<HEAD>\n"
                         "<TITLE>Error</TITLE>\n"
                         "<META HTTP-EQUIV='Content-Type' CONTENT='text/html; charset=ISO-8859-1'>\n"
                         "</HEAD>\n"
                         "<BODY><H1>401 Unauthorized.</H1></BODY>\n"
                         "</HTML>\n",
            rfc1123Time(), FormatFullVersion());

    return HTTPReply(nStatus, httpStatusDescription(nStatus), keepalive,
        headersOnly, "text/plain");
}

string HTTPReplyHeader(int nStatus, bool keepalive, size_t contentLength, const char* contentType)
{
    return strprintf(
        "HTTP/1.1 %d %s\n"
        "Date: %s\n"
        "Connection: %s\n"
        "Content-Length: %u\n"
        "Content-Type: %s\n"
        "Server: nodex-json-rpc/%s\n"
        "\n",
        nStatus,
        httpStatusDescription(nStatus),
        rfc1123Time(),
        keepalive ? "keep-alive" : "close",
        contentLength,
        contentType,
        FormatFullVersion());
}

string HTTPReply(int nStatus, const string& strMsg, bool keepalive, bool headersOnly, const char* contentType)
{
    if (headersOnly) {
        return HTTPReplyHeader(nStatus, keepalive, 0, contentType);
    } else {
        return HTTPReplyHeader(nStatus, keepalive, strMsg.size(), contentType) + strMsg;
    }
}

bool ReadHTTPRequestLine(std::basic_istream<char>& stream, int& proto, string& http_method, string& http_uri)
{
    string str;
    getline(stream, str);

    // HTTP request line is space-delimited
    vector<string> vWords;
    boost::split(vWords, str, boost::is_any_of(" "));
    if (vWords.size() < 2)
        return false;

    // HTTP methods permitted: GET, POST
    http_method = vWords[0];
    if (http_method != "GET" && http_method != "POST")
        return false;

    // HTTP URI must be an absolute path, relative to current host
    http_uri = vWords[1];
    if (http_uri.size() == 0 || http_uri[0] != '/')
        return false;

    // parse proto, if present
    string strProto = "";
    if (vWords.size() > 2)
        strProto = vWords[2];

    proto = 0;
    const char* ver = strstr(strProto.c_str(), "HTTP/1.");
    if (ver != NULL)
        proto = atoi(ver + 7);

    return true;
}

int ReadHTTPStatus(std::basic_istream<char>& stream, int& proto)
{
    string str;
    getline(stream, str);
    //LogPrintf("ReadHTTPStatus - getline string: %s\n",str.c_str());
    vector<string> vWords;
    boost::split(vWords, str, boost::is_any_of(" "));
    if (vWords.size() < 2)
        return HTTP_INTERNAL_SERVER_ERROR;
    proto = 0;
    const char* ver = strstr(str.c_str(), "HTTP/1.");
    if (ver != NULL)
        proto = atoi(ver + 7);
    return atoi(vWords[1].c_str());
}

int ReadHTTPHeaders(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet)
{
    int nLen = 0;
    while (true) {
        string str;
        std::getline(stream, str);
        if (str.empty() || str == "")
            break;
        string::size_type nColon = str.find(":");
        if (nColon != string::npos) {
            string strHeader = str.substr(0, nColon);
            boost::trim(strHeader);
            boost::to_lower(strHeader);
            string strValue = str.substr(nColon + 1);
            boost::trim(strValue);
            mapHeadersRet[strHeader] = strValue;
            if (strHeader == "content-length")
                nLen = atoi(strValue.c_str());
        }
    }
    return nLen;
}


int ReadHTTPMessage(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet, string& strMessageRet, int nProto, size_t max_size)
{
    mapHeadersRet.clear();
    strMessageRet = "";

    // Read header
    int nLen = ReadHTTPHeaders(stream, mapHeadersRet);
    if (nLen < 0 || (size_t)nLen > max_size)
        return HTTP_INTERNAL_SERVER_ERROR;

    // Read message
    if (nLen > 0) {
        vector<char> vch;
        size_t ptr = 0;
        while (ptr < (size_t)nLen) {
            size_t bytes_to_read = std::min((size_t)nLen - ptr, POST_READ_SIZE);
            vch.resize(ptr + bytes_to_read);
            stream.read(&vch[ptr], bytes_to_read);
            if (!stream) // Connection lost while reading
                return HTTP_INTERNAL_SERVER_ERROR;
            ptr += bytes_to_read;
        }
        strMessageRet = string(vch.begin(), vch.end());
    }

    string sConHdr = mapHeadersRet["connection"];

    if ((sConHdr != "close") && (sConHdr != "keep-alive")) {
        if (nProto >= 1)
            mapHeadersRet["connection"] = "keep-alive";
        else
            mapHeadersRet["connection"] = "close";
    }

    return HTTP_OK;
}

/**
 * JSON-RPC protocol.  Nodex speaks version 1.0 for maximum compatibility,
 * but uses JSON-RPC 1.1/2.0 standards for parts of the 1.0 standard that were
 * unspecified (HTTP errors and contents of 'error').
 * 
 * 1.0 spec: http://json-rpc.org/wiki/specification
 * 1.2 spec: http://jsonrpc.org/historical/json-rpc-over-http.html
 * http://www.codeproject.com/KB/recipes/JSON_Spirit.aspx
 */

string JSONRPCRequest(const string& strMethod, const Array& params, const Value& id)
{
    Object request;
    request.push_back(Pair("method", strMethod));
    request.push_back(Pair("params", params));
    request.push_back(Pair("id", id));
    return write_string(Value(request), false) + "\n";
}

Object JSONRPCReplyObj(const Value& result, const Value& error, const Value& id)
{
    Object reply;
    if (error.type() != null_type)
        reply.push_back(Pair("result", Value::null));
    else
        reply.push_back(Pair("result", result));
    reply.push_back(Pair("error", error));
    reply.push_back(Pair("id", id));
    return reply;
}

string JSONRPCReply(const Value& result, const Value& error, const Value& id)
{
    Object reply = JSONRPCReplyObj(result, error, id);
    return write_string(Value(reply), false) + "\n";
}

Object JSONRPCError(int code, const string& message)
{
    Object error;
    error.push_back(Pair("code", code));
    error.push_back(Pair("message", message));
    return error;
}

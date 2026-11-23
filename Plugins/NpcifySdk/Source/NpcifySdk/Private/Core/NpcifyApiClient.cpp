#include "Core/NpcifyApiClient.h"

#include "Core/NpcifyJson.h"

#include <stdexcept>
#include <utility>

namespace Npcify
{
    namespace
    {
        std::string DescribeResponseFailure(const std::string& Prefix, const FNpcifyHttpResponse& Response)
        {
            if (!Response.Error.empty())
            {
                return Prefix + ": " + Response.Error;
            }
            FNpcifyMsgPackValue Parsed;
            if (!Response.Body.empty() && FNpcifyJson::TryParse(Response.Body, Parsed) && Parsed.IsMap())
            {
                const FNpcifyMsgPackValue* Msg = Parsed.FindField("message");
                if (Msg && Msg->IsString() && !Msg->StringValue.empty())
                {
                    return Prefix + " (HTTP " + std::to_string(Response.StatusCode) + "). " + Msg->StringValue;
                }
            }
            return Prefix + " (HTTP " + std::to_string(Response.StatusCode) + ").";
        }
    }

    FNpcifyApiClient::FNpcifyApiClient(const FNpcifyOptions& InOptions, FTransportFunc InTransport)
        : Options(InOptions)
        , Transport(std::move(InTransport))
    {
    }

    std::string FNpcifyApiClient::NormalizeBaseUrl(const std::string& BaseUrl)
    {
        std::string Trimmed = BaseUrl.empty() ? std::string("https://api.npcify.ai") : BaseUrl;
        while (!Trimmed.empty() && Trimmed.back() == '/')
        {
            Trimmed.pop_back();
        }
        return Trimmed;
    }

    std::map<std::string, std::string> FNpcifyApiClient::BuildAuthHeaders(
        const std::string& Token,
        const std::string& Accept
    )
    {
        std::map<std::string, std::string> Headers;
        if (!Accept.empty())
        {
            Headers["Accept"] = Accept;
        }
        if (!Token.empty())
        {
            Headers["Authorization"] = "Bearer " + Token;
            Headers["X-API-Token"] = Token;
        }
        return Headers;
    }

    std::string FNpcifyApiClient::BuildAvatarInfoUrl(const std::string& BaseUrl, const std::string& AvatarId)
    {
        if (AvatarId.empty())
        {
            throw std::invalid_argument("AvatarId must not be empty.");
        }
        return NormalizeBaseUrl(BaseUrl) + "/avatars/" + FNpcifyProtocol::UrlEncode(AvatarId) + "/info";
    }

    std::string FNpcifyApiClient::BuildVrmDownloadUrlEndpoint(
        const std::string& BaseUrl,
        const std::string& AvatarId,
        const std::string& AssetProfile
    )
    {
        if (AvatarId.empty())
        {
            throw std::invalid_argument("AvatarId must not be empty.");
        }
        std::string Url = NormalizeBaseUrl(BaseUrl) + "/avatars/" + FNpcifyProtocol::UrlEncode(AvatarId) + "/download-url";
        if (!AssetProfile.empty())
        {
            Url += "?assetProfile=" + FNpcifyProtocol::UrlEncode(AssetProfile);
        }
        return Url;
    }

    std::string FNpcifyApiClient::BuildVariantVrmDownloadUrlEndpoint(
        const std::string& BaseUrl,
        const std::string& AvatarId,
        const std::string& VariantKey,
        const std::string& AssetProfile
    )
    {
        if (AvatarId.empty())
        {
            throw std::invalid_argument("AvatarId must not be empty.");
        }
        if (!FNpcifyProtocol::IsValidVariantKey(VariantKey))
        {
            throw std::invalid_argument("Invalid variant key: " + VariantKey);
        }
        if (VariantKey == FNpcifyProtocol::DefaultVariantKey)
        {
            return BuildVrmDownloadUrlEndpoint(BaseUrl, AvatarId, AssetProfile);
        }
        std::string Url = NormalizeBaseUrl(BaseUrl) +
            "/avatars/" + FNpcifyProtocol::UrlEncode(AvatarId) +
            "/variants/" + FNpcifyProtocol::UrlEncode(VariantKey) +
            "/download-url";
        if (!AssetProfile.empty())
        {
            Url += "?assetProfile=" + FNpcifyProtocol::UrlEncode(AssetProfile);
        }
        return Url;
    }

    std::string FNpcifyApiClient::BuildWorldlineUrl(const std::string& BaseUrl, const std::string& ConversationId)
    {
        if (ConversationId.empty())
        {
            throw std::invalid_argument("ConversationId must not be empty.");
        }
        return NormalizeBaseUrl(BaseUrl) + "/conversations/" + FNpcifyProtocol::UrlEncode(ConversationId) + "/worldline";
    }

    std::string FNpcifyApiClient::ParseDownloadUrlResponse(const std::string& ResponseJson)
    {
        const FNpcifyMsgPackValue Parsed = FNpcifyJson::Parse(ResponseJson);
        if (!Parsed.IsMap())
        {
            throw std::runtime_error("Expected JSON object in download-url response.");
        }
        const FNpcifyMsgPackValue* UrlField = Parsed.FindField("url");
        if (!UrlField || !UrlField->IsString() || UrlField->StringValue.empty())
        {
            throw std::runtime_error("Download URL response is missing a non-empty url field.");
        }
        return UrlField->StringValue;
    }

    FNpcifyAvatarInfo FNpcifyApiClient::ParseAvatarInfo(const std::string& ResponseJson)
    {
        return FNpcifySchemaMapper::MapAvatarInfo(FNpcifyJson::Parse(ResponseJson));
    }

    FConversationWorldline FNpcifyApiClient::ParseWorldline(const std::string& ResponseJson)
    {
        return FNpcifySchemaMapper::MapWorldline(FNpcifyJson::Parse(ResponseJson));
    }

    void FNpcifyApiClient::DispatchGetJson(const std::string& Url, FResponseCallback Callback) const
    {
        if (!Transport)
        {
            throw std::runtime_error("FNpcifyApiClient has no HTTP transport configured.");
        }
        FNpcifyHttpRequest Req;
        Req.Method = "GET";
        Req.Url = Url;
        Req.Headers = BuildAuthHeaders(Options.ResolvedToken(), "application/json");
        Transport(Req, std::move(Callback));
    }

    void FNpcifyApiClient::GetVrmDownloadUrl(
        const std::string& AvatarId,
        const std::string& AssetProfile,
        FUrlResultCallback Callback
    ) const
    {
        const std::string TargetId = !AvatarId.empty() ? AvatarId : Options.CharacterId;
        const std::string Url = BuildVrmDownloadUrlEndpoint(Options.BaseUrl, TargetId, AssetProfile);
        DispatchGetJson(Url, [Callback = std::move(Callback)](const FNpcifyHttpResponse& Resp)
        {
            if (!Callback)
            {
                return;
            }
            if (!Resp.IsOk())
            {
                Callback("", DescribeResponseFailure("Failed to fetch VRM download URL", Resp));
                return;
            }
            try
            {
                Callback(ParseDownloadUrlResponse(Resp.Body), "");
            }
            catch (const std::exception& Ex)
            {
                Callback("", Ex.what());
            }
        });
    }

    std::string FNpcifyApiClient::GetVrmDownloadUrl(
        const std::string& AvatarId,
        const std::string& AssetProfile
    ) const
    {
        std::string ResolvedUrl;
        std::string ResolvedError;
        bool bInvoked = false;
        GetVrmDownloadUrl(AvatarId, AssetProfile, [&](const std::string& Url, const std::string& Error)
        {
            bInvoked = true;
            ResolvedUrl = Url;
            ResolvedError = Error;
        });
        if (!bInvoked)
        {
            throw std::runtime_error("HTTP transport did not complete synchronously.");
        }
        if (!ResolvedError.empty())
        {
            throw std::runtime_error(ResolvedError);
        }
        return ResolvedUrl;
    }

    void FNpcifyApiClient::GetVariantVrmDownloadUrl(
        const std::string& AvatarId,
        const std::string& VariantKey,
        const std::string& AssetProfile,
        FUrlResultCallback Callback
    ) const
    {
        const std::string TargetId = !AvatarId.empty() ? AvatarId : Options.CharacterId;
        const std::string Url = BuildVariantVrmDownloadUrlEndpoint(Options.BaseUrl, TargetId, VariantKey, AssetProfile);
        DispatchGetJson(Url, [Callback = std::move(Callback)](const FNpcifyHttpResponse& Resp)
        {
            if (!Callback)
            {
                return;
            }
            if (!Resp.IsOk())
            {
                Callback("", DescribeResponseFailure("Failed to fetch variant VRM download URL", Resp));
                return;
            }
            try
            {
                Callback(ParseDownloadUrlResponse(Resp.Body), "");
            }
            catch (const std::exception& Ex)
            {
                Callback("", Ex.what());
            }
        });
    }

    std::string FNpcifyApiClient::GetVariantVrmDownloadUrl(
        const std::string& AvatarId,
        const std::string& VariantKey,
        const std::string& AssetProfile
    ) const
    {
        std::string ResolvedUrl;
        std::string ResolvedError;
        bool bInvoked = false;
        GetVariantVrmDownloadUrl(AvatarId, VariantKey, AssetProfile, [&](const std::string& Url, const std::string& Error)
        {
            bInvoked = true;
            ResolvedUrl = Url;
            ResolvedError = Error;
        });
        if (!bInvoked)
        {
            throw std::runtime_error("HTTP transport did not complete synchronously.");
        }
        if (!ResolvedError.empty())
        {
            throw std::runtime_error(ResolvedError);
        }
        return ResolvedUrl;
    }

    void FNpcifyApiClient::GetAvatarInfo(const std::string& AvatarId, FAvatarInfoCallback Callback) const
    {
        const std::string TargetId = !AvatarId.empty() ? AvatarId : Options.CharacterId;
        const std::string Url = BuildAvatarInfoUrl(Options.BaseUrl, TargetId);
        DispatchGetJson(Url, [Callback = std::move(Callback)](const FNpcifyHttpResponse& Resp)
        {
            if (!Callback)
            {
                return;
            }
            if (!Resp.IsOk())
            {
                Callback(FNpcifyAvatarInfo{}, DescribeResponseFailure("Failed to fetch avatar info", Resp));
                return;
            }
            try
            {
                Callback(ParseAvatarInfo(Resp.Body), "");
            }
            catch (const std::exception& Ex)
            {
                Callback(FNpcifyAvatarInfo{}, Ex.what());
            }
        });
    }

    FNpcifyAvatarInfo FNpcifyApiClient::GetAvatarInfo(const std::string& AvatarId) const
    {
        FNpcifyAvatarInfo Out;
        std::string ResolvedError;
        bool bInvoked = false;
        GetAvatarInfo(AvatarId, [&](const FNpcifyAvatarInfo& Info, const std::string& Error)
        {
            bInvoked = true;
            Out = Info;
            ResolvedError = Error;
        });
        if (!bInvoked)
        {
            throw std::runtime_error("HTTP transport did not complete synchronously.");
        }
        if (!ResolvedError.empty())
        {
            throw std::runtime_error(ResolvedError);
        }
        return Out;
    }

    void FNpcifyApiClient::GetConversationWorldline(
        const std::string& ConversationId,
        FWorldlineCallback Callback
    ) const
    {
        const std::string Url = BuildWorldlineUrl(Options.BaseUrl, ConversationId);
        DispatchGetJson(Url, [Callback = std::move(Callback)](const FNpcifyHttpResponse& Resp)
        {
            if (!Callback)
            {
                return;
            }
            if (!Resp.IsOk())
            {
                Callback(FConversationWorldline{}, DescribeResponseFailure("Failed to fetch conversation worldline", Resp));
                return;
            }
            try
            {
                Callback(ParseWorldline(Resp.Body), "");
            }
            catch (const std::exception& Ex)
            {
                Callback(FConversationWorldline{}, Ex.what());
            }
        });
    }

    FConversationWorldline FNpcifyApiClient::GetConversationWorldline(const std::string& ConversationId) const
    {
        FConversationWorldline Out;
        std::string ResolvedError;
        bool bInvoked = false;
        GetConversationWorldline(ConversationId, [&](const FConversationWorldline& Worldline, const std::string& Error)
        {
            bInvoked = true;
            Out = Worldline;
            ResolvedError = Error;
        });
        if (!bInvoked)
        {
            throw std::runtime_error("HTTP transport did not complete synchronously.");
        }
        if (!ResolvedError.empty())
        {
            throw std::runtime_error(ResolvedError);
        }
        return Out;
    }
}

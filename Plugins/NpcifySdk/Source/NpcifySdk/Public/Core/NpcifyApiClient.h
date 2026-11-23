#pragma once

#include "Core/NpcifyModels.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Npcify
{
    struct FNpcifyHttpRequest
    {
        std::string Method = "GET";
        std::string Url;
        std::map<std::string, std::string> Headers;
        std::vector<uint8_t> Body;
    };

    struct FNpcifyHttpResponse
    {
        int StatusCode = 0;
        std::string Body;
        std::vector<uint8_t> BinaryBody;
        std::string Error;

        bool IsOk() const
        {
            return Error.empty() && StatusCode >= 200 && StatusCode < 300;
        }
    };

    /**
     * Platform-neutral HTTP client helper for character metadata, worldline inspection,
     * and VRM / variant download-URL resolution.
     */
    class FNpcifyApiClient
    {
    public:
        using FResponseCallback = std::function<void(const FNpcifyHttpResponse&)>;
        using FTransportFunc = std::function<void(const FNpcifyHttpRequest&, FResponseCallback)>;
        using FUrlResultCallback = std::function<void(const std::string& Url, const std::string& Error)>;
        using FAvatarInfoCallback = std::function<void(const FNpcifyAvatarInfo& Info, const std::string& Error)>;
        using FWorldlineCallback = std::function<void(const FConversationWorldline& Worldline, const std::string& Error)>;

        explicit FNpcifyApiClient(const FNpcifyOptions& InOptions, FTransportFunc InTransport = nullptr);

        const FNpcifyOptions& GetOptions() const { return Options; }
        void SetOptions(const FNpcifyOptions& InOptions) { Options = InOptions; }
        void SetTransport(FTransportFunc InTransport) { Transport = std::move(InTransport); }

        static std::string NormalizeBaseUrl(const std::string& BaseUrl);
        static std::map<std::string, std::string> BuildAuthHeaders(
            const std::string& Token,
            const std::string& Accept = "application/json"
        );

        static std::string BuildAvatarInfoUrl(const std::string& BaseUrl, const std::string& AvatarId);
        static std::string BuildVrmDownloadUrlEndpoint(
            const std::string& BaseUrl,
            const std::string& AvatarId,
            const std::string& AssetProfile = ""
        );
        static std::string BuildVariantVrmDownloadUrlEndpoint(
            const std::string& BaseUrl,
            const std::string& AvatarId,
            const std::string& VariantKey,
            const std::string& AssetProfile = ""
        );
        static std::string BuildWorldlineUrl(const std::string& BaseUrl, const std::string& ConversationId);

        static std::string ParseDownloadUrlResponse(const std::string& ResponseJson);
        static FNpcifyAvatarInfo ParseAvatarInfo(const std::string& ResponseJson);
        static FConversationWorldline ParseWorldline(const std::string& ResponseJson);

        void GetVrmDownloadUrl(
            const std::string& AvatarId,
            const std::string& AssetProfile,
            FUrlResultCallback Callback
        ) const;
        std::string GetVrmDownloadUrl(
            const std::string& AvatarId,
            const std::string& AssetProfile = ""
        ) const;

        void GetVariantVrmDownloadUrl(
            const std::string& AvatarId,
            const std::string& VariantKey,
            const std::string& AssetProfile,
            FUrlResultCallback Callback
        ) const;
        std::string GetVariantVrmDownloadUrl(
            const std::string& AvatarId,
            const std::string& VariantKey = FNpcifyProtocol::DefaultVariantKey,
            const std::string& AssetProfile = ""
        ) const;

        void GetAvatarInfo(const std::string& AvatarId, FAvatarInfoCallback Callback) const;
        FNpcifyAvatarInfo GetAvatarInfo(const std::string& AvatarId) const;

        void GetConversationWorldline(const std::string& ConversationId, FWorldlineCallback Callback) const;
        FConversationWorldline GetConversationWorldline(const std::string& ConversationId) const;

    private:
        void DispatchGetJson(const std::string& Url, FResponseCallback Callback) const;

        FNpcifyOptions Options;
        FTransportFunc Transport;
    };
}

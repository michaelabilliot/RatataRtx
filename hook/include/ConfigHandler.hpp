#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

enum class DisplayModes {
    Windowed,
    Borderless,
    Fullscreen
};

struct RatataRConfig {
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int maxFps = 0;
    unsigned int fov = 95;
    bool console = false;
    bool popupMenu = false;
    bool invertVerticalLook = false;
    bool removeFpsCap = false;
    bool autoSave = true;
    bool improvedViewDistance = false;
    bool fog = true;
    bool noBonks = false;
    bool speedrunMode = false;
    bool discordRichPresence = false;
    bool displayFrameCounter = false;
    bool rtxRemixBridge = true;
    bool rtxDebugLog = false;
    bool rtxFailOpen = true;
    bool rtxBridgeLogFile = true;
    bool rtxAllowUnsafeShaderWvp = false;
    bool rtxAllowUnsafeRuntimeReplace = false;
    std::string rtxBridgeSubmissionMode = "originalOnly";
    std::string rtxTransformMode = "rendererMatrixCache";
    std::string rtxWvpRegister = "auto";
    std::string rtxWvpTranspose = "auto";
    bool rtxAutoLockWvpRegister = true;
    std::string rtxVertexBridgeMode = "repackAuto";
    unsigned int rtxRepackMaxVertices = 65536;
    std::string rtxLightBridgeMode = "off";
    std::string rtxFixedFunctionMaterialMode = "captureNeutral";
    float rtxShaderMaterialEmissiveScale = 0.0f;
    bool rtxCaptureAllowTexturelessProxy = false;
    bool rtxMatrixProbeLog = false;
    std::string rtxProxyGeometryFilter = "solidWorldOnly";
    bool rtxProxyDeduplicateCommandDraws = true;
    std::string rtxTransformAssemblyMode = "splitWorldViewProjection";
    std::string rtxProxyAlphaTestMode = "solidWorldMasked";
    std::string rtxProxySkinnedMode = "paletteSkinning";
    std::string rtxProxyCameraLockMode = "multiCandidate";
    std::string rtxProxyUiSkyMode = "classifiedProxy";
    DisplayModes displayMode = DisplayModes::Borderless;
};

struct RemyFOV {
    float normal = 95.0f;
    float climbing = 110.0f;
    double runningSliding = 110.0;
};

RemyFOV getFOVValues(unsigned int desired_fov);

class ConfigHandler {
public:
    ConfigHandler() = default;

    void load(const std::string& path);
    void save(const std::string& path) const;

    const RatataRConfig& getConfig() const;
    RatataRConfig& getConfig();

private:
    RatataRConfig cfg;
    std::string configPath;

    template<typename T>
    void LoadOption(const char* section, const char* key, T RatataRConfig::* member) {
        if constexpr (std::is_same_v<T, bool>) {
            char buffer[32]{};
            GetPrivateProfileStringA(
                section,
                key,
                cfg.*member ? "1" : "0",
                buffer,
                sizeof(buffer),
                configPath.c_str()
            );
            cfg.*member = ParseBool(buffer, cfg.*member);
        }
        else if constexpr (std::is_integral_v<T>) {
            cfg.*member = GetPrivateProfileIntA(section, key, cfg.*member, configPath.c_str());
        }
        else if constexpr (std::is_floating_point_v<T>) {
            char fallback[32]{};
            snprintf(fallback, sizeof(fallback), "%.6g", static_cast<double>(cfg.*member));
            char buffer[64]{};
            GetPrivateProfileStringA(
                section,
                key,
                fallback,
                buffer,
                sizeof(buffer),
                configPath.c_str()
            );
            char* end = nullptr;
            const float parsed = std::strtof(buffer, &end);
            if (end != buffer) {
                cfg.*member = static_cast<T>(parsed);
            }
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            char buffer[128]{};
            GetPrivateProfileStringA(
                section,
                key,
                (cfg.*member).c_str(),
                buffer,
                sizeof(buffer),
                configPath.c_str()
            );
            cfg.*member = buffer;
        }
    }

    template<typename T>
    void SaveOption(const char* section, const char* key, T value) const {
        char buffer[64];
        if constexpr (std::is_same_v<T, bool> || std::is_integral_v<T>) {
            snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(value));
            WritePrivateProfileStringA(section, key, buffer, configPath.c_str());
        }
        else if constexpr (std::is_floating_point_v<T>) {
            snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
            WritePrivateProfileStringA(section, key, buffer, configPath.c_str());
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            WritePrivateProfileStringA(section, key, value.c_str(), configPath.c_str());
        }
    }

    static bool ParseBool(const char* value, bool fallback);
    static DisplayModes ParseDisplayMode(const char* value);
};

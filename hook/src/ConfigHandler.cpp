#include "ConfigHandler.hpp"
#include <Windows.h>
#include <string>
#include <cctype>

void ConfigHandler::load(const std::string& path) {
    configPath = path;
    constexpr char section[] = "CONFIG";
    
    // Integer options
    LoadOption(section, "width", &RatataRConfig::width);
    LoadOption(section, "height", &RatataRConfig::height);
    LoadOption(section, "maxFps", &RatataRConfig::maxFps);
    LoadOption(section, "fov", &RatataRConfig::fov);

    // Clamp FOV
    constexpr unsigned int fovMin = 1;
    constexpr unsigned int fovMax = 155;
    if (cfg.fov < fovMin) cfg.fov = fovMin;
    else if (cfg.fov > fovMax) cfg.fov = fovMax;

    // Boolean options
    LoadOption(section, "console", &RatataRConfig::console);
    LoadOption(section, "popupMenu", &RatataRConfig::popupMenu);
    LoadOption(section, "invertVerticalLook", &RatataRConfig::invertVerticalLook);
    LoadOption(section, "removeFpsCap", &RatataRConfig::removeFpsCap);
    LoadOption(section, "autoSave", &RatataRConfig::autoSave);
    LoadOption(section, "improvedViewDistance", &RatataRConfig::improvedViewDistance);
    LoadOption(section, "fog", &RatataRConfig::fog);
    LoadOption(section, "noBonks", &RatataRConfig::noBonks);
    LoadOption(section, "speedrunMode", &RatataRConfig::speedrunMode);
    LoadOption(section, "discordRichPresence", &RatataRConfig::discordRichPresence);
    LoadOption(section, "displayFrameCounter", &RatataRConfig::displayFrameCounter);
    LoadOption(section, "rtxRemixBridge", &RatataRConfig::rtxRemixBridge);
    LoadOption(section, "rtxDebugLog", &RatataRConfig::rtxDebugLog);
    LoadOption(section, "rtxFailOpen", &RatataRConfig::rtxFailOpen);
    LoadOption(section, "rtxBridgeLogFile", &RatataRConfig::rtxBridgeLogFile);
    LoadOption(section, "rtxAllowUnsafeShaderWvp", &RatataRConfig::rtxAllowUnsafeShaderWvp);
    LoadOption(section, "rtxAllowUnsafeRuntimeReplace", &RatataRConfig::rtxAllowUnsafeRuntimeReplace);
    LoadOption(section, "rtxBridgeSubmissionMode", &RatataRConfig::rtxBridgeSubmissionMode);
    LoadOption(section, "rtxTransformMode", &RatataRConfig::rtxTransformMode);
    LoadOption(section, "rtxWvpRegister", &RatataRConfig::rtxWvpRegister);
    LoadOption(section, "rtxWvpTranspose", &RatataRConfig::rtxWvpTranspose);
    LoadOption(section, "rtxAutoLockWvpRegister", &RatataRConfig::rtxAutoLockWvpRegister);
    LoadOption(section, "rtxVertexBridgeMode", &RatataRConfig::rtxVertexBridgeMode);
    LoadOption(section, "rtxRepackMaxVertices", &RatataRConfig::rtxRepackMaxVertices);
    LoadOption(section, "rtxLightBridgeMode", &RatataRConfig::rtxLightBridgeMode);
    LoadOption(section, "rtxFixedFunctionMaterialMode", &RatataRConfig::rtxFixedFunctionMaterialMode);
    LoadOption(section, "rtxShaderMaterialEmissiveScale", &RatataRConfig::rtxShaderMaterialEmissiveScale);
    LoadOption(section, "rtxCaptureAllowTexturelessProxy", &RatataRConfig::rtxCaptureAllowTexturelessProxy);
    LoadOption(section, "rtxMatrixProbeLog", &RatataRConfig::rtxMatrixProbeLog);
    LoadOption(section, "rtxProxyGeometryFilter", &RatataRConfig::rtxProxyGeometryFilter);
    LoadOption(section, "rtxProxyDeduplicateCommandDraws", &RatataRConfig::rtxProxyDeduplicateCommandDraws);
    LoadOption(section, "rtxTransformAssemblyMode", &RatataRConfig::rtxTransformAssemblyMode);
    LoadOption(section, "rtxProxyAlphaTestMode", &RatataRConfig::rtxProxyAlphaTestMode);
    LoadOption(section, "rtxProxySkinnedMode", &RatataRConfig::rtxProxySkinnedMode);
    LoadOption(section, "rtxProxyCameraLockMode", &RatataRConfig::rtxProxyCameraLockMode);
    LoadOption(section, "rtxProxyUiSkyMode", &RatataRConfig::rtxProxyUiSkyMode);

    // Get display mode
    char displayModeBuffer[32];
    GetPrivateProfileStringA(
        section,
        "displayMode",
        "BORDERLESS",
        displayModeBuffer,
        sizeof(displayModeBuffer),
        path.c_str()
    );
    cfg.displayMode = ParseDisplayMode(displayModeBuffer);
}

void ConfigHandler::save(const std::string& path) const {
    constexpr char section[] = "CONFIG";

    // Int
    SaveOption(section, "width", cfg.width);
    SaveOption(section, "height", cfg.height);
    SaveOption(section, "maxFps", cfg.maxFps);
    SaveOption(section, "fov", cfg.fov);

    // Bool
    SaveOption(section, "console", cfg.console);
    SaveOption(section, "popupMenu", cfg.popupMenu);
    SaveOption(section, "invertVerticalLook", cfg.invertVerticalLook);
    SaveOption(section, "removeFpsCap", cfg.removeFpsCap);
    SaveOption(section, "autoSave", cfg.autoSave);
    SaveOption(section, "improvedViewDistance", cfg.improvedViewDistance);
    SaveOption(section, "fog", cfg.fog);
    SaveOption(section, "noBonks", cfg.noBonks);
    SaveOption(section, "speedrunMode", cfg.speedrunMode);
    SaveOption(section, "discordRichPresence", cfg.discordRichPresence);
    SaveOption(section, "displayFrameCounter", cfg.displayFrameCounter);
    SaveOption(section, "rtxRemixBridge", cfg.rtxRemixBridge);
    SaveOption(section, "rtxDebugLog", cfg.rtxDebugLog);
    SaveOption(section, "rtxFailOpen", cfg.rtxFailOpen);
    SaveOption(section, "rtxBridgeLogFile", cfg.rtxBridgeLogFile);
    SaveOption(section, "rtxAllowUnsafeShaderWvp", cfg.rtxAllowUnsafeShaderWvp);
    SaveOption(section, "rtxAllowUnsafeRuntimeReplace", cfg.rtxAllowUnsafeRuntimeReplace);
    SaveOption(section, "rtxBridgeSubmissionMode", cfg.rtxBridgeSubmissionMode);
    SaveOption(section, "rtxTransformMode", cfg.rtxTransformMode);
    SaveOption(section, "rtxWvpRegister", cfg.rtxWvpRegister);
    SaveOption(section, "rtxWvpTranspose", cfg.rtxWvpTranspose);
    SaveOption(section, "rtxAutoLockWvpRegister", cfg.rtxAutoLockWvpRegister);
    SaveOption(section, "rtxVertexBridgeMode", cfg.rtxVertexBridgeMode);
    SaveOption(section, "rtxRepackMaxVertices", cfg.rtxRepackMaxVertices);
    SaveOption(section, "rtxLightBridgeMode", cfg.rtxLightBridgeMode);
    SaveOption(section, "rtxFixedFunctionMaterialMode", cfg.rtxFixedFunctionMaterialMode);
    SaveOption(section, "rtxShaderMaterialEmissiveScale", cfg.rtxShaderMaterialEmissiveScale);
    SaveOption(section, "rtxCaptureAllowTexturelessProxy", cfg.rtxCaptureAllowTexturelessProxy);
    SaveOption(section, "rtxMatrixProbeLog", cfg.rtxMatrixProbeLog);
    SaveOption(section, "rtxProxyGeometryFilter", cfg.rtxProxyGeometryFilter);
    SaveOption(section, "rtxProxyDeduplicateCommandDraws", cfg.rtxProxyDeduplicateCommandDraws);
    SaveOption(section, "rtxTransformAssemblyMode", cfg.rtxTransformAssemblyMode);
    SaveOption(section, "rtxProxyAlphaTestMode", cfg.rtxProxyAlphaTestMode);
    SaveOption(section, "rtxProxySkinnedMode", cfg.rtxProxySkinnedMode);
    SaveOption(section, "rtxProxyCameraLockMode", cfg.rtxProxyCameraLockMode);
    SaveOption(section, "rtxProxyUiSkyMode", cfg.rtxProxyUiSkyMode);

    std::string modeStr;
    switch (cfg.displayMode) {
        case DisplayModes::Windowed: modeStr = "Windowed"; break;
        case DisplayModes::Fullscreen: modeStr = "Fullscreen"; break;
        default: modeStr = "Borderless";
    }

    WritePrivateProfileStringA(section, "displayMode", modeStr.c_str(), path.c_str());
}

const RatataRConfig& ConfigHandler::getConfig() const { return cfg; }
RatataRConfig& ConfigHandler::getConfig() { return cfg; }

RemyFOV getFOVValues(unsigned int desired_fov) {
    constexpr float CLIMBING_RATIO = 110.0f / 95.0f;
    constexpr double RUNNING_SLIDING_RATIO = 110.0 / 95.0;

    float userFOV = static_cast<float>(desired_fov);
    return {
        .normal = userFOV,
        .climbing = CLIMBING_RATIO * userFOV,
        .runningSliding = static_cast<float>(RUNNING_SLIDING_RATIO * userFOV)
    };
}

bool ConfigHandler::ParseBool(const char* value, bool fallback) {
    std::string upper;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (!std::isspace(static_cast<unsigned char>(value[i]))) {
            upper += static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
        }
    }

    if (upper == "1" || upper == "TRUE" || upper == "YES" || upper == "ON") return true;
    if (upper == "0" || upper == "FALSE" || upper == "NO" || upper == "OFF") return false;
    return fallback;
}

DisplayModes ConfigHandler::ParseDisplayMode(const char* value) {
    std::string upper;
    for (size_t i = 0; value[i] != '\0'; i++) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    }

    if (upper == "WINDOWED") return DisplayModes::Windowed;
    if (upper == "FULLSCREEN") return DisplayModes::Fullscreen;
    return DisplayModes::Borderless;
}

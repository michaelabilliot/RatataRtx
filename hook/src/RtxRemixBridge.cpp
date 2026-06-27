#include "RtxRemixBridge.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <type_traits>
#include <d3d9.h>
#include <intrin.h>

#include "MinHook.h"
#include "MemoryUtils.hpp"
#include "SigScanner.hpp"
#include "SignaturePatterns.hpp"

namespace RtxRemixBridge
{
    namespace
    {
        constexpr std::ptrdiff_t RENDERER_DEVICE_OFFSET = 0x2570;
        constexpr std::ptrdiff_t RENDERER_CONSTANT_CACHE_OFFSET = 0x6CD540;
        constexpr std::ptrdiff_t RENDERER_CONSTANT3_SLOT_OFFSET = 0x5C;
        constexpr std::ptrdiff_t RENDERER_CONSTANT4_SLOT_OFFSET = 0x60;
        constexpr std::ptrdiff_t RENDERER_CONSTANT5_SLOT_OFFSET = 0x64;
        constexpr std::ptrdiff_t RENDERER_CONSTANT_MATRIX_OFFSET = 0x08;
        constexpr std::size_t MAX_LOGGED_DECLARATIONS = 64;
        constexpr std::size_t MAX_LOGGED_MATRICES = 16;
        constexpr std::size_t MAX_MATRIX_PROBE_CANDIDATES = 64;
        constexpr std::size_t MAX_LOGGED_REPACK_LAYOUTS = 64;
        constexpr std::size_t MAX_LOGGED_PROXY_EVENTS = 96;
        constexpr UINT SHADER_CONSTANT_REGISTER_COUNT = 96;
        constexpr DWORD SCRATCH_VERTEX_FVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
        constexpr UINT RTX_CAPTURE_LIGHT_COUNT = 6;
        constexpr UINT RTX_SHADER_LIGHT_SETS_PER_FRAME = 1;
        constexpr float RTX_SHADER_MATERIAL_EMISSIVE_MAX = 0.05f;
        constexpr float RTX_SHADER_LIGHT_COLOR_MAX = 2.0f;
        constexpr float RTX_SHADER_LIGHT_AMBIENT_MAX = 0.30f;
        constexpr float RTX_SHADER_LIGHT_RANGE_MAX = 2500.0f;

        struct D3DRendererZ
        {
            IDirect3DDevice9* Device() const
            {
                return *reinterpret_cast<IDirect3DDevice9* const*>(
                    reinterpret_cast<const std::uint8_t*>(this) + RENDERER_DEVICE_OFFSET);
            }
        };

#pragma pack(push, 1)
        struct RenderCommand188
        {
            std::uint8_t sortKey[0x10];
            void* primitiveDraw;
            void* material;
            std::uint32_t stateBlock[11];
            std::uint8_t constants[0x70];
            std::int32_t nextOrPrev;
            std::int32_t nextOrPrev2;
        };

        struct PrimitiveDraw
        {
            void* vertexBuffer;
            void* indexBuffer;
            void* shaderConstant;
            std::uint16_t primitiveType;
            std::uint16_t splitCountKey;
            std::uint16_t unused10;
            std::uint16_t minVertexIndex;
            std::uint16_t numVertices;
            std::uint16_t startIndex;
            std::uint16_t primitiveCount;
            std::uint16_t indexBindParam;
            std::uint16_t startVertex;
            std::uint16_t stride;
        };
#pragma pack(pop)

        static_assert(sizeof(RenderCommand188) == 0xBC);
        static_assert(offsetof(RenderCommand188, primitiveDraw) == 0x10);
        static_assert(offsetof(RenderCommand188, material) == 0x14);
        static_assert(offsetof(RenderCommand188, stateBlock) == 0x18);
        static_assert(offsetof(RenderCommand188, constants) == 0x44);
        static_assert(offsetof(PrimitiveDraw, primitiveType) == 0x0C);
        static_assert(offsetof(PrimitiveDraw, splitCountKey) == 0x0E);
        static_assert(offsetof(PrimitiveDraw, minVertexIndex) == 0x12);
        static_assert(offsetof(PrimitiveDraw, primitiveCount) == 0x18);
        static_assert(offsetof(PrimitiveDraw, startVertex) == 0x1C);
        static_assert(offsetof(PrimitiveDraw, stride) == 0x1E);

        struct ModuleInfo
        {
            std::uintptr_t base = 0;
            std::size_t size = 0;
        };

        enum class RtxTransformMode
        {
            ShaderConstantsAuto,
            ShaderConstantsRegister,
            ShaderConstantsWvp,
            RatatouilleShaderConstants,
            RendererConstant5,
            RendererSlot5World,
            RendererMatrixCache,
            DeviceTransforms
        };

        enum class BridgeSubmissionMode
        {
            OriginalOnly,
            ProxyAfterOriginal,
            CaptureProxyAfterOriginal,
            VisibleProxyAfterOriginal,
            RuntimeMirrorAfterOriginal,
            RuntimeFixedFunction,
            ReplaceVisible
        };

        enum class WvpTransposeMode
        {
            Auto,
            ForceFalse,
            ForceTrue
        };

        enum class VertexBridgeMode
        {
            RepackAuto,
            DirectFvf
        };

        enum class LightBridgeMode
        {
            DebugDirectional,
            ShaderConstants,
            Off
        };

        enum class FixedFunctionMaterialMode
        {
            CaptureNeutral,
            TexturelessWhite,
            TextureFullbright
        };

        enum class ProxyGeometryFilterMode
        {
            SolidWorldOnly,
            AllSupported
        };

        enum class TransformAssemblyMode
        {
            SplitWorldViewProjection,
            FusedWorldView,
            FullWvpProjection
        };

        enum class ProxyAlphaTestMode
        {
            SolidWorldMasked,
            Skip
        };

        enum class ProxySkinnedMode
        {
            Off,
            PaletteSkinning
        };

        enum class ProxyCameraLockMode
        {
            Single,
            MultiCandidate
        };

        enum class ProxyUiSkyMode
        {
            Off,
            ClassifiedProxy
        };

        enum class ProxyDrawClass
        {
            World,
            Ui,
            Sky
        };

        enum class FixedFunctionPassStatus
        {
            ConvertedDirect,
            ConvertedRepacked,
            Unsupported,
            SkippedUi
        };

        enum class FixedFunctionPassPath
        {
            None,
            DirectFvf,
            Repacked
        };

        enum class FixedFunctionPassReason
        {
            None,
            ConvertedDirect,
            ConvertedRepacked,
            SkippedPassMode,
            SkippedZDisabled,
            SkippedLikelyUi,
            SkippedRhw,
            SkippedScreenMatrix,
            NoTransform,
            BadAffine,
            BadViewProjection,
            BadVertexBounds,
            NoWvpCandidate,
            InvalidIndexRange,
            CollapsedTransform,
            DirectFvfOnlyFailed,
            NonTrianglePrimitive,
            RepackLayoutFailed,
            MissingTexcoord,
            SkippedSkinned,
            SkippedPatch,
            SkippedTransparent,
            SkippedScreenCopy,
            SkippedDuplicateCommand,
            SkippedCameraMismatch,
            SkippedScreenPlane,
            SkippedProjectedOversize,
            VertexCountRejected,
            RepackSourceUnavailable,
            RepackLockFailed,
            StreamOverrideFailed,
            DrawFailed,
            Unsupported
        };

        struct BridgeConfig
        {
            bool enabled = true;
            bool debugLog = false;
            bool failOpen = true;
            bool logFile = true;
            bool allowUnsafeShaderWvp = false;
            bool allowUnsafeRuntimeReplace = false;
            BridgeSubmissionMode submissionMode = BridgeSubmissionMode::OriginalOnly;
            RtxTransformMode transformMode = RtxTransformMode::RendererMatrixCache;
            WvpTransposeMode wvpTransposeMode = WvpTransposeMode::Auto;
            VertexBridgeMode vertexBridgeMode = VertexBridgeMode::RepackAuto;
            LightBridgeMode lightBridgeMode = LightBridgeMode::Off;
            FixedFunctionMaterialMode fixedFunctionMaterialMode = FixedFunctionMaterialMode::CaptureNeutral;
            float shaderMaterialEmissiveScale = 0.0f;
            bool captureAllowTexturelessProxy = false;
            bool matrixProbeLog = false;
            ProxyGeometryFilterMode proxyGeometryFilter = ProxyGeometryFilterMode::SolidWorldOnly;
            bool proxyDeduplicateCommandDraws = true;
            TransformAssemblyMode transformAssemblyMode = TransformAssemblyMode::SplitWorldViewProjection;
            ProxyAlphaTestMode proxyAlphaTestMode = ProxyAlphaTestMode::SolidWorldMasked;
            ProxySkinnedMode proxySkinnedMode = ProxySkinnedMode::PaletteSkinning;
            ProxyCameraLockMode proxyCameraLockMode = ProxyCameraLockMode::MultiCandidate;
            ProxyUiSkyMode proxyUiSkyMode = ProxyUiSkyMode::ClassifiedProxy;
            bool autoLockWvpRegister = true;
            int wvpRegister = -1;
            UINT repackMaxVertices = 65536;
        };

        struct BridgeAddresses
        {
            std::uintptr_t queueDrain = 0;
            std::uintptr_t renderCommand = 0;
            std::uintptr_t drawPrimitiveBuffer = 0;
            std::uintptr_t drawPrimitiveUp = 0;
            std::uintptr_t setStream = 0;
            std::uintptr_t setIndices = 0;

            std::uintptr_t applyShaderConstant = 0;
            std::uintptr_t computeSplitCount = 0;
            std::uintptr_t advanceDrawPass = 0;
            std::uintptr_t matrixMultiplyHelper = 0;
            std::uintptr_t lookAtHelper = 0;
        };

        struct BridgeCounters
        {
            std::uint64_t queueDrains = 0;
            std::uint64_t commands = 0;
            std::uint64_t drawsSeen = 0;
            std::uint64_t convertedDraws = 0;
            std::uint64_t passthroughDraws = 0;
            std::uint64_t unsupportedDraws = 0;
            std::uint64_t skippedDraws = 0;
            std::uint64_t uiSkippedDraws = 0;
            std::uint64_t rhwSkippedDraws = 0;
            std::uint64_t zDisabledSkippedDraws = 0;
            std::uint64_t primitiveUpDraws = 0;
            std::uint64_t fvfFailures = 0;
            std::uint64_t commandWvpMissing = 0;
            std::uint64_t directFvfDraws = 0;
            std::uint64_t repackedDraws = 0;
            std::uint64_t repackFallbacks = 0;
            std::uint64_t matrixCandidates = 0;
            std::uint64_t matrixFailures = 0;
            std::uint64_t shaderWvpCandidates = 0;
            std::uint64_t shaderWvpSelected = 0;
            std::uint64_t shaderWvpRejectedLogs = 0;
            std::uint64_t ratTransformCandidates = 0;
            std::uint64_t ratTransformSelected = 0;
            std::uint64_t ratTransformRejected = 0;
            std::uint64_t matrixProbeCaptured = 0;
            std::uint64_t matrixProbeSelected = 0;
            std::uint64_t matrixProbeRejected = 0;
            std::uint64_t wvpNoCandidate = 0;
            std::uint64_t wvpFallbackWorld = 0;
            std::uint64_t runtimeAttempts = 0;
            std::uint64_t runtimeSubmitted = 0;
            std::uint64_t invalidIndexRanges = 0;
            std::uint64_t badRuntimeTransforms = 0;
            std::uint64_t debugLightDraws = 0;
            std::uint64_t shaderMaterialDraws = 0;
            std::uint64_t shaderLightsSeen = 0;
            std::uint64_t shaderLegacyLightsSubmitted = 0;
            std::uint64_t shaderDirectionalLights = 0;
            std::uint64_t shaderPointLights = 0;
            std::uint64_t shaderSpotLights = 0;
            std::uint64_t shaderLightSetsApplied = 0;
            std::uint64_t shaderLightSetsChanged = 0;
            std::uint64_t shaderLightSetsReused = 0;
            std::uint64_t shaderLightsSuppressed = 0;
            std::uint64_t screenMatrixSkips = 0;
            std::uint64_t noWorldMatrixFallbacks = 0;
            std::uint64_t noTexcoordFallbacks = 0;
            std::uint64_t nonTriangleFallbacks = 0;
            std::uint64_t repackNormalsCopied = 0;
            std::uint64_t repackNormalsGenerated = 0;
            std::uint64_t repackNormalsDefaulted = 0;
            std::uint64_t originalOnlyDraws = 0;
            std::uint64_t proxyAttempts = 0;
            std::uint64_t proxySubmitted = 0;
            std::uint64_t proxySkippedNoTransform = 0;
            std::uint64_t proxySkippedBadAffine = 0;
            std::uint64_t proxySkippedBadViewProj = 0;
            std::uint64_t proxySkippedBadBounds = 0;
            std::uint64_t proxySkippedPassMode = 0;
            std::uint64_t proxySubmittedDirect = 0;
            std::uint64_t proxySubmittedRepacked = 0;
            std::uint64_t proxySkippedZDisabled = 0;
            std::uint64_t proxySkippedRhw = 0;
            std::uint64_t proxySkippedScreenMatrix = 0;
            std::uint64_t proxyUnsupportedFvf = 0;
            std::uint64_t proxyUnsupportedRepack = 0;
            std::uint64_t proxySkippedNoTexcoord = 0;
            std::uint64_t proxySkippedNonTriangle = 0;
            std::uint64_t proxySkippedSkinned = 0;
            std::uint64_t proxySkippedPatch = 0;
            std::uint64_t proxySkippedTransparent = 0;
            std::uint64_t proxySkippedScreenCopy = 0;
            std::uint64_t proxySkippedDuplicateCommand = 0;
            std::uint64_t proxySkippedCameraMismatch = 0;
            std::uint64_t proxySkippedScreenPlane = 0;
            std::uint64_t proxySkippedProjectedOversize = 0;
            std::uint64_t proxyAlphaTestSubmitted = 0;
            std::uint64_t proxyAlphaTestRejected = 0;
            std::uint64_t proxyDrawFailures = 0;
            std::uint64_t visibleReplacementBlocked = 0;
            std::uint64_t skinnedSubmitted = 0;
            std::uint64_t skinnedPaletteMissing = 0;
            std::uint64_t uiProxySubmitted = 0;
            std::uint64_t skyProxySubmitted = 0;
            std::uint64_t cameraCandidateAccepted = 0;
            std::uint64_t cameraCandidateRejected = 0;
            std::uint64_t rendererTripletValid = 0;
            std::uint64_t rendererTripletInvalid = 0;
            std::uint64_t rendererTripletSubmitted = 0;
            std::uint64_t maskedWorldSubmitted = 0;
            std::uint64_t uiRhwSubmitted = 0;
            std::uint64_t uiOrthographicSubmitted = 0;
            std::uint64_t uiHeuristicRejected = 0;
            std::uint64_t skyWorldSubmitted = 0;
            std::uint64_t skyHeuristicRejected = 0;
            std::uint64_t cameraRelativeRejected = 0;
            std::uint64_t skinnedPaletteComplete = 0;
            std::uint64_t skinnedPaletteIncomplete = 0;
        };

        struct FvfBuildResult
        {
            bool ok = false;
            bool rhw = false;
            DWORD fvf = 0;
            int mappedStride = 0;
            char reason[160]{};
            UINT elementCount = 0;
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH]{};
        };

        struct FixedFunctionPassResult
        {
            FixedFunctionPassStatus status = FixedFunctionPassStatus::Unsupported;
            HRESULT hr = D3DERR_INVALIDCALL;
            FixedFunctionPassPath path = FixedFunctionPassPath::None;
            FixedFunctionPassReason reason = FixedFunctionPassReason::None;
            bool hasTexture = false;
            bool directFvfOk = false;
            bool transformOk = false;
            bool transformTransposed = false;
            bool transformIsWorld = false;
            bool proxyOnly = false;
            bool visibleProxy = false;
            bool colorWritesEnabled = true;
            bool noOpBlend = false;
            bool alphaTestEnabled = false;
            ProxyDrawClass proxyClass = ProxyDrawClass::World;
            bool renderStateSnapshotValid = false;
            DWORD colorWriteMask = D3DCOLORWRITEENABLE_RED |
                D3DCOLORWRITEENABLE_GREEN |
                D3DCOLORWRITEENABLE_BLUE |
                D3DCOLORWRITEENABLE_ALPHA;
            DWORD zEnable = D3DZB_TRUE;
            DWORD zWriteEnable = TRUE;
            DWORD alphaBlendEnable = FALSE;
            DWORD srcBlend = 0;
            DWORD dstBlend = 0;
            UINT textureWidth = 0;
            UINT textureHeight = 0;
            DWORD textureUsage = 0;
            bool textureScreenSized = false;
            bool textureRenderTarget = false;
            DWORD fvf = 0;
            int mappedStride = 0;
            char detail[192]{};
            char fvfReason[160]{};
            char repackReason[160]{};
            char transformSource[32]{};
            char transformReason[160]{};
            char shaderLayout[32]{};
        };

        struct MatrixSelection
        {
            bool ok = false;
            D3DMATRIX matrix{};
            int startRegister = -1;
            bool transposed = false;
            bool screenRejected = false;
            bool affineRejected = false;
            float score = 0.0f;
            char source[32]{};
            char reason[160]{};
        };

        struct VertexBounds
        {
            bool valid = false;
            float minX = 0.0f;
            float minY = 0.0f;
            float minZ = 0.0f;
            float maxX = 0.0f;
            float maxY = 0.0f;
            float maxZ = 0.0f;
        };

        struct Stage0TextureInfo
        {
            bool hasTexture = false;
            bool screenSized = false;
            bool renderTarget = false;
            bool viewportSized = false;
            bool backbufferSized = false;
            UINT width = 0;
            UINT height = 0;
            DWORD usage = 0;
            D3DPOOL pool = D3DPOOL_DEFAULT;
            D3DFORMAT format = D3DFMT_UNKNOWN;
        };

        struct ProjectedBoundsMetrics
        {
            bool valid = false;
            int finiteCorners = 0;
            float minNdcX = 0.0f;
            float minNdcY = 0.0f;
            float maxNdcX = 0.0f;
            float maxNdcY = 0.0f;
            float extentX = 0.0f;
            float extentY = 0.0f;
            float area = 0.0f;
            float maxAbsNdc = 0.0f;
        };

        struct MatrixProbeCandidate
        {
            bool valid = false;
            D3DMATRIX matrix{};
            std::uintptr_t caller = 0;
            std::uint64_t sequence = 0;
            char source[32]{};
        };

        struct RepackLayout
        {
            bool ok = false;
            bool rhw = false;
            bool hasDiffuse = false;
            bool hasTexcoord = false;
            bool hasNormal = false;
            bool hasNormalSemantic = false;
            bool hasBlendIndices = false;
            bool hasBlendWeight = false;
            bool hasPosition1 = false;
            bool hasPosition2 = false;
            bool hasPosition3 = false;
            bool hasPatchLikePositionSet = false;
            bool hasColorSemantic = false;
            int positionOffset = -1;
            int diffuseOffset = -1;
            int texcoordOffset = -1;
            int texcoordComponents = 0;
            int normalOffset = -1;
            int normalType = D3DDECLTYPE_UNUSED;
            int blendIndexOffset = -1;
            int blendIndexType = D3DDECLTYPE_UNUSED;
            int blendIndexComponents = 0;
            int blendWeightOffset = -1;
            int blendWeightType = D3DDECLTYPE_UNUSED;
            int blendWeightComponents = 0;
            UINT elementCount = 0;
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH]{};
            char reason[160]{};
        };

        struct FrameCameraLock
        {
            bool valid = false;
            D3DMATRIX view{};
            D3DMATRIX projection{};
            D3DVIEWPORT9 viewport{};
            std::uint32_t hits = 0;
        };

        struct ScratchVertex
        {
            float x;
            float y;
            float z;
            float nx;
            float ny;
            float nz;
            DWORD diffuse;
            float u;
            float v;
        };

        struct FixedFunctionTransformSet
        {
            bool valid = false;
            bool setWorld = false;
            bool setView = false;
            bool setProjection = false;
            D3DMATRIX world{};
            D3DMATRIX view{};
            D3DMATRIX projection{};
        };

        struct ShaderConstantsSnapshot
        {
            bool valid = false;
            std::array<float, SHADER_CONSTANT_REGISTER_COUNT * 4> values{};
        };

        enum class SkinningPaletteEncoding
        {
            Matrix3x4,
            QuaternionTranslation
        };

        struct SkinningPaletteSelection
        {
            bool ok = false;
            bool indicesAreRegisters = false;
            int baseRegister = -1;
            int secondaryBaseRegister = -1;
            int influenceCount = 0;
            float score = 0.0f;
            SkinningPaletteEncoding encoding = SkinningPaletteEncoding::Matrix3x4;
            char reason[160]{};
        };

        struct RatatouilleShaderLayout
        {
            const char* name;
            int viewRegister;
            int projectionRegister;
            int worldRegister;
            int materialRegister;
            int lightRegister;
            int omniRegister;
            int omniSize;
            int omniCount;
        };

        struct RatatouilleTransformSelection
        {
            bool ok = false;
            bool transposed = false;
            bool screenRejected = false;
            bool affineRejected = false;
            float score = 0.0f;
            const RatatouilleShaderLayout* layout = nullptr;
            FixedFunctionTransformSet transforms{};
            char reason[160]{};
        };

        constexpr RatatouilleShaderLayout RATATOUILLE_SHADER_LAYOUTS[] = {
            { "ratatouilleDefault", 18, 22, 26, 31, 37, 47, 4, 5 },
            { "ratatouilleNoScattering", 5, 9, 13, 18, 24, 34, 4, 5 },
        };

        using QueueDrainFn = int(__thiscall*)(D3DRendererZ*, int);
        using RenderCommandFn = char(__thiscall*)(D3DRendererZ*, RenderCommand188*);
        using DrawPrimitiveBufferFn = char(__thiscall*)(D3DRendererZ*, PrimitiveDraw*, int);
        using DrawPrimitiveUpFn = int(__thiscall*)(D3DRendererZ*, int, std::uint16_t, int, int, int, int);
        using ApplyShaderConstantFn = int(__thiscall*)(void*, IDirect3DDevice9*);
        using SetStreamFn = int(__thiscall*)(D3DRendererZ*, void*, std::uint16_t);
        using SetIndicesFn = int(__thiscall*)(D3DRendererZ*, void*, int);
        using ComputeSplitCountFn = int(__thiscall*)(D3DRendererZ*, std::uint16_t, int);
        using AdvanceDrawPassFn = int(__thiscall*)(D3DRendererZ*);
        using MatrixMultiplyHelperFn = int(__thiscall*)(const D3DMATRIX*, D3DMATRIX*, const D3DMATRIX*);
        using LookAtHelperFn = int(__cdecl*)(const void*, const void*, const void*, D3DMATRIX*);

        BridgeConfig g_config{};
        BridgeAddresses g_addresses{};
        BridgeCounters g_counters{};
        bool g_installed = false;
        std::string g_logPath;
        std::array<std::uint32_t, MAX_LOGGED_DECLARATIONS> g_loggedDeclarationHashes{};
        std::size_t g_loggedDeclarationCount = 0;
        std::uint64_t g_suppressedDeclarationLogs = 0;
        std::array<std::uint32_t, MAX_LOGGED_MATRICES> g_loggedMatrixHashes{};
        std::size_t g_loggedMatrixCount = 0;
        std::array<MatrixProbeCandidate, MAX_MATRIX_PROBE_CANDIDATES> g_matrixProbeCandidates{};
        std::size_t g_matrixProbeCursor = 0;
        std::uint64_t g_matrixProbeSequence = 0;
        std::array<std::uint32_t, MAX_LOGGED_REPACK_LAYOUTS> g_loggedRepackHashes{};
        std::size_t g_loggedRepackCount = 0;
        std::array<std::uint32_t, MAX_LOGGED_PROXY_EVENTS> g_loggedProxyEventHashes{};
        std::size_t g_loggedProxyEventCount = 0;
        std::uint64_t g_suppressedProxyEventLogs = 0;
        bool g_loggedFirstWvp = false;
        IDirect3DDevice9* g_scratchDevice = nullptr;
        IDirect3DVertexBuffer9* g_scratchVertexBuffer = nullptr;
        UINT g_scratchVertexCapacity = 0;
        bool g_lockedWvpValid = false;
        int g_lockedWvpRegister = -1;
        bool g_lockedWvpTransposed = false;
        bool g_shaderLightHashValid = false;
        std::uint32_t g_shaderLightHash = 0;
        UINT g_shaderLightSetsAppliedThisFrame = 0;
        constexpr std::size_t MAX_FRAME_CAMERA_CANDIDATES = 8;
        std::array<FrameCameraLock, MAX_FRAME_CAMERA_CANDIDATES> g_frameCameraCandidates{};
        std::size_t g_frameCameraCandidateCount = 0;
        RenderCommand188* g_proxyDedupeCommand = nullptr;
        std::array<std::uint32_t, 128> g_proxyDedupeHashes{};
        std::size_t g_proxyDedupeHashCount = 0;

        QueueDrainFn g_originalQueueDrain = nullptr;
        RenderCommandFn g_originalRenderCommand = nullptr;
        DrawPrimitiveBufferFn g_originalDrawPrimitiveBuffer = nullptr;
        DrawPrimitiveUpFn g_originalDrawPrimitiveUp = nullptr;
        ApplyShaderConstantFn g_applyShaderConstant = nullptr;
        SetStreamFn g_setStream = nullptr;
        SetIndicesFn g_setIndices = nullptr;
        ComputeSplitCountFn g_computeSplitCount = nullptr;
        AdvanceDrawPassFn g_advanceDrawPass = nullptr;
        MatrixMultiplyHelperFn g_originalMatrixMultiplyHelper = nullptr;
        LookAtHelperFn g_originalLookAtHelper = nullptr;

        thread_local RenderCommand188* g_currentCommand = nullptr;

        template<typename T>
        void HashValue(std::uint32_t& hash, T value);

        std::string GetHookDirectory()
        {
            HMODULE module = nullptr;
            char modulePath[MAX_PATH]{};
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(&GetHookDirectory),
                    &module) &&
                GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
                char* lastSlash = std::strrchr(modulePath, '\\');
                if (!lastSlash) {
                    lastSlash = std::strrchr(modulePath, '/');
                }

                if (lastSlash) {
                    *lastSlash = '\0';
                    return modulePath;
                }
            }

            char currentDirectory[MAX_PATH]{};
            if (GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory)) {
                return currentDirectory;
            }

            return ".";
        }

        std::string AppendPath(const std::string& base, const char* leaf)
        {
            if (base.empty()) {
                return leaf;
            }

            const char last = base.back();
            if (last == '\\' || last == '/') {
                return base + leaf;
            }

            return base + "\\" + leaf;
        }

        std::string GetEnvironmentPath(const char* name)
        {
            char buffer[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
            if (length == 0 || length >= sizeof(buffer)) {
                return {};
            }

            return buffer;
        }

        std::string GetTempDirectory()
        {
            char buffer[MAX_PATH]{};
            const DWORD length = GetTempPathA(sizeof(buffer), buffer);
            if (length == 0 || length >= sizeof(buffer)) {
                return {};
            }

            return buffer;
        }

        bool EnsureDirectoryExists(const std::string& path)
        {
            if (path.empty()) {
                return false;
            }

            if (CreateDirectoryA(path.c_str(), nullptr)) {
                return true;
            }

            return GetLastError() == ERROR_ALREADY_EXISTS;
        }

        bool ProbeWritableLogPath(const std::string& path)
        {
            HANDLE file = CreateFileA(
                path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (file == INVALID_HANDLE_VALUE) {
                return false;
            }

            CloseHandle(file);
            return true;
        }

        void WriteLogLine(const char* line)
        {
            if (g_config.logFile && !g_logPath.empty()) {
                HANDLE file = CreateFileA(
                    g_logPath.c_str(),
                    FILE_APPEND_DATA,
                    FILE_SHARE_READ,
                    nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

                if (file != INVALID_HANDLE_VALUE) {
                    DWORD bytesWritten = 0;
                    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &bytesWritten, nullptr);
                    CloseHandle(file);
                }
            }

            if (g_config.debugLog) {
                OutputDebugStringA(line);
            }
        }

        void LogFormatted(const char* fmt, va_list args)
        {
            char body[1024]{};
            vsnprintf(body, sizeof(body), fmt, args);

            char line[1120]{};
            snprintf(line, sizeof(line), "[RatataR RTX Remix] %s\n", body);
            WriteLogLine(line);
        }

        void LogAlways(const char* fmt, ...)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            va_list args;
            va_start(args, fmt);
            LogFormatted(fmt, args);
            va_end(args);
        }

        void DebugLog(const char* fmt, ...)
        {
            if (!g_config.debugLog) {
                return;
            }

            va_list args;
            va_start(args, fmt);
            LogFormatted(fmt, args);
            va_end(args);
        }

        void InitializeFileLog()
        {
            g_logPath.clear();
            if (!g_config.logFile) {
                return;
            }

            std::array<std::string, 3> candidates{};
            std::size_t candidateCount = 0;

            const std::string localAppData = GetEnvironmentPath("LOCALAPPDATA");
            if (!localAppData.empty()) {
                const std::string ratatarDir = AppendPath(localAppData, "RatataR");
                if (EnsureDirectoryExists(ratatarDir)) {
                    candidates[candidateCount++] = AppendPath(ratatarDir, "RatataR-rtx-bridge.log");
                }
            }

            const std::string tempDir = GetTempDirectory();
            if (!tempDir.empty()) {
                candidates[candidateCount++] = AppendPath(tempDir, "RatataR-rtx-bridge.log");
            }

            candidates[candidateCount++] = AppendPath(GetHookDirectory(), "RatataR-rtx-bridge.log");

            for (std::size_t i = 0; i < candidateCount; ++i) {
                if (ProbeWritableLogPath(candidates[i])) {
                    g_logPath = candidates[i];
                    return;
                }
            }
        }

        bool HasActiveDxvkSetting(const std::string& content, const char* key)
        {
            std::size_t lineStart = 0;
            const std::size_t keyLength = std::strlen(key);

            while (lineStart < content.size()) {
                std::size_t lineEnd = content.find_first_of("\r\n", lineStart);
                if (lineEnd == std::string::npos) {
                    lineEnd = content.size();
                }

                std::size_t cursor = lineStart;
                while (cursor < lineEnd && (content[cursor] == ' ' || content[cursor] == '\t')) {
                    ++cursor;
                }

                if (cursor < lineEnd && content[cursor] != '#' && content[cursor] != ';' &&
                    lineEnd - cursor >= keyLength &&
                    _strnicmp(content.c_str() + cursor, key, keyLength) == 0) {
                    cursor += keyLength;
                    while (cursor < lineEnd && (content[cursor] == ' ' || content[cursor] == '\t')) {
                        ++cursor;
                    }

                    if (cursor < lineEnd && content[cursor] == '=') {
                        return true;
                    }
                }

                lineStart = lineEnd + 1;
                while (lineStart < content.size() && (content[lineStart] == '\r' || content[lineStart] == '\n')) {
                    ++lineStart;
                }
            }

            return false;
        }

        void CheckDxvkConfig()
        {
            const std::string envPath = GetEnvironmentPath("DXVK_CONFIG_FILE");
            const bool usingEnvPath = !envPath.empty();
            const std::string path = usingEnvPath ? envPath : AppendPath(GetHookDirectory(), "dxvk.conf");
            LogAlways("dxvk config path: %s%s", path.c_str(), usingEnvPath ? " (DXVK_CONFIG_FILE)" : "");

            HANDLE file = CreateFileA(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (file == INVALID_HANDLE_VALUE) {
                LogAlways("dxvk warning: %s not found", path.c_str());
                return;
            }

            const DWORD fileSize = GetFileSize(file, nullptr);
            if (fileSize == INVALID_FILE_SIZE || fileSize > 1024 * 1024) {
                CloseHandle(file);
                LogAlways("dxvk warning: cannot read %s size=%lu", path.c_str(), static_cast<unsigned long>(fileSize));
                return;
            }

            std::string content(fileSize, '\0');
            DWORD bytesRead = 0;
            if (fileSize != 0 && !ReadFile(file, content.data(), fileSize, &bytesRead, nullptr)) {
                CloseHandle(file);
                LogAlways("dxvk warning: ReadFile failed for %s", path.c_str());
                return;
            }
            CloseHandle(file);
            content.resize(bytesRead);

            constexpr const char* requiredSettings[] = {
                "d3d9.psShaderModel",
                "d3d9.vsShaderModel",
                "rtx.useUnusedRenderstates",
                "rtx.vertexColorStrength",
                "rtx.useVertexCapture",
                "rtx.useVertexCapturedNormals",
                "rtx.useWorldMatricesForShaders",
                "rtx.orthographicIsUI"
            };

            bool missingAny = false;
            for (const char* setting : requiredSettings) {
                if (!HasActiveDxvkSetting(content, setting)) {
                    LogAlways("dxvk warning: missing active setting %s", setting);
                    missingAny = true;
                }
            }

            if (!missingAny) {
                LogAlways("dxvk config check passed");
            }
        }

        RtxTransformMode ParseTransformMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "shaderConstantsRegister") == 0) {
                return RtxTransformMode::ShaderConstantsRegister;
            }

            if (_stricmp(value.c_str(), "shaderConstantsWvp") == 0) {
                return RtxTransformMode::ShaderConstantsWvp;
            }

            if (_stricmp(value.c_str(), "ratatouilleShaderConstants") == 0 ||
                _stricmp(value.c_str(), "shaderConstantLayout") == 0) {
                return RtxTransformMode::RatatouilleShaderConstants;
            }

            if (_stricmp(value.c_str(), "rendererSlot5World") == 0) {
                return RtxTransformMode::RendererSlot5World;
            }

            if (_stricmp(value.c_str(), "rendererMatrixCache") == 0) {
                return RtxTransformMode::RendererMatrixCache;
            }

            if (_stricmp(value.c_str(), "rendererConstant5") == 0) {
                return RtxTransformMode::RendererConstant5;
            }

            if (_stricmp(value.c_str(), "deviceTransforms") == 0) {
                return RtxTransformMode::DeviceTransforms;
            }

            return RtxTransformMode::ShaderConstantsAuto;
        }

        const char* TransformModeName(RtxTransformMode mode)
        {
            switch (mode) {
                case RtxTransformMode::ShaderConstantsRegister: return "shaderConstantsRegister";
                case RtxTransformMode::ShaderConstantsWvp: return "shaderConstantsWvp";
                case RtxTransformMode::RatatouilleShaderConstants: return "ratatouilleShaderConstants";
                case RtxTransformMode::RendererConstant5: return "rendererConstant5";
                case RtxTransformMode::RendererSlot5World: return "rendererSlot5World";
                case RtxTransformMode::RendererMatrixCache: return "rendererMatrixCache";
                case RtxTransformMode::DeviceTransforms: return "deviceTransforms";
                default: return "shaderConstantsAuto";
            }
        }

        BridgeSubmissionMode ParseSubmissionMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "proxyAfterOriginal") == 0) {
                return BridgeSubmissionMode::ProxyAfterOriginal;
            }

            if (_stricmp(value.c_str(), "captureProxyAfterOriginal") == 0) {
                return BridgeSubmissionMode::CaptureProxyAfterOriginal;
            }

            if (_stricmp(value.c_str(), "visibleProxyAfterOriginal") == 0) {
                return BridgeSubmissionMode::VisibleProxyAfterOriginal;
            }

            if (_stricmp(value.c_str(), "runtimeMirrorAfterOriginal") == 0) {
                return BridgeSubmissionMode::RuntimeMirrorAfterOriginal;
            }

            if (_stricmp(value.c_str(), "runtimeFixedFunction") == 0) {
                return BridgeSubmissionMode::RuntimeFixedFunction;
            }

            if (_stricmp(value.c_str(), "replaceVisible") == 0) {
                return BridgeSubmissionMode::ReplaceVisible;
            }

            return BridgeSubmissionMode::OriginalOnly;
        }

        const char* SubmissionModeName(BridgeSubmissionMode mode)
        {
            switch (mode) {
                case BridgeSubmissionMode::ProxyAfterOriginal: return "proxyAfterOriginal";
                case BridgeSubmissionMode::CaptureProxyAfterOriginal: return "captureProxyAfterOriginal";
                case BridgeSubmissionMode::VisibleProxyAfterOriginal: return "visibleProxyAfterOriginal";
                case BridgeSubmissionMode::RuntimeMirrorAfterOriginal: return "runtimeMirrorAfterOriginal";
                case BridgeSubmissionMode::RuntimeFixedFunction: return "runtimeFixedFunction";
                case BridgeSubmissionMode::ReplaceVisible: return "replaceVisible";
                default: return "originalOnly";
            }
        }

        WvpTransposeMode ParseWvpTransposeMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "true") == 0 || _stricmp(value.c_str(), "1") == 0 ||
                _stricmp(value.c_str(), "yes") == 0 || _stricmp(value.c_str(), "on") == 0) {
                return WvpTransposeMode::ForceTrue;
            }

            if (_stricmp(value.c_str(), "false") == 0 || _stricmp(value.c_str(), "0") == 0 ||
                _stricmp(value.c_str(), "no") == 0 || _stricmp(value.c_str(), "off") == 0) {
                return WvpTransposeMode::ForceFalse;
            }

            return WvpTransposeMode::Auto;
        }

        const char* WvpTransposeModeName(WvpTransposeMode mode)
        {
            switch (mode) {
                case WvpTransposeMode::ForceTrue: return "true";
                case WvpTransposeMode::ForceFalse: return "false";
                default: return "auto";
            }
        }

        VertexBridgeMode ParseVertexBridgeMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "directFvf") == 0) {
                return VertexBridgeMode::DirectFvf;
            }

            return VertexBridgeMode::RepackAuto;
        }

        const char* VertexBridgeModeName(VertexBridgeMode mode)
        {
            return mode == VertexBridgeMode::DirectFvf ? "directFvf" : "repackAuto";
        }

        LightBridgeMode ParseLightBridgeMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "shaderConstants") == 0) {
                return LightBridgeMode::ShaderConstants;
            }

            if (_stricmp(value.c_str(), "off") == 0) {
                return LightBridgeMode::Off;
            }

            return LightBridgeMode::DebugDirectional;
        }

        const char* LightBridgeModeName(LightBridgeMode mode)
        {
            switch (mode) {
                case LightBridgeMode::ShaderConstants: return "shaderConstants";
                case LightBridgeMode::Off: return "off";
                default: return "debugDirectional";
            }
        }

        FixedFunctionMaterialMode ParseFixedFunctionMaterialMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "captureNeutral") == 0) {
                return FixedFunctionMaterialMode::CaptureNeutral;
            }

            if (_stricmp(value.c_str(), "textureFullbright") == 0) {
                return FixedFunctionMaterialMode::TextureFullbright;
            }

            if (_stricmp(value.c_str(), "texturelessWhite") == 0) {
                return FixedFunctionMaterialMode::TexturelessWhite;
            }

            return FixedFunctionMaterialMode::CaptureNeutral;
        }

        const char* FixedFunctionMaterialModeName(FixedFunctionMaterialMode mode)
        {
            switch (mode) {
                case FixedFunctionMaterialMode::CaptureNeutral: return "captureNeutral";
                case FixedFunctionMaterialMode::TextureFullbright: return "textureFullbright";
                default: return "texturelessWhite";
            }
        }

        ProxyGeometryFilterMode ParseProxyGeometryFilterMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "allSupported") == 0 ||
                _stricmp(value.c_str(), "off") == 0 ||
                _stricmp(value.c_str(), "none") == 0) {
                return ProxyGeometryFilterMode::AllSupported;
            }

            return ProxyGeometryFilterMode::SolidWorldOnly;
        }

        const char* ProxyGeometryFilterModeName(ProxyGeometryFilterMode mode)
        {
            return mode == ProxyGeometryFilterMode::AllSupported ? "allSupported" : "solidWorldOnly";
        }

        TransformAssemblyMode ParseTransformAssemblyMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "fusedWorldView") == 0) {
                return TransformAssemblyMode::FusedWorldView;
            }

            if (_stricmp(value.c_str(), "fullWvpProjection") == 0 ||
                _stricmp(value.c_str(), "wvpProjection") == 0) {
                return TransformAssemblyMode::FullWvpProjection;
            }

            return TransformAssemblyMode::SplitWorldViewProjection;
        }

        const char* TransformAssemblyModeName(TransformAssemblyMode mode)
        {
            switch (mode) {
                case TransformAssemblyMode::FusedWorldView: return "fusedWorldView";
                case TransformAssemblyMode::FullWvpProjection: return "fullWvpProjection";
                default: return "splitWorldViewProjection";
            }
        }

        ProxyAlphaTestMode ParseProxyAlphaTestMode(const std::string& value)
        {
            if (_stricmp(value.c_str(), "skip") == 0 ||
                _stricmp(value.c_str(), "off") == 0 ||
                _stricmp(value.c_str(), "reject") == 0) {
                return ProxyAlphaTestMode::Skip;
            }

            return ProxyAlphaTestMode::SolidWorldMasked;
        }

        const char* ProxyAlphaTestModeName(ProxyAlphaTestMode mode)
        {
            return mode == ProxyAlphaTestMode::Skip ? "skip" : "solidWorldMasked";
        }

        ProxySkinnedMode ParseProxySkinnedMode(const std::string& value)
        {
            return _stricmp(value.c_str(), "paletteSkinning") == 0
                ? ProxySkinnedMode::PaletteSkinning
                : ProxySkinnedMode::Off;
        }

        const char* ProxySkinnedModeName(ProxySkinnedMode mode)
        {
            return mode == ProxySkinnedMode::PaletteSkinning ? "paletteSkinning" : "off";
        }

        ProxyCameraLockMode ParseProxyCameraLockMode(const std::string& value)
        {
            return _stricmp(value.c_str(), "multiCandidate") == 0
                ? ProxyCameraLockMode::MultiCandidate
                : ProxyCameraLockMode::Single;
        }

        const char* ProxyCameraLockModeName(ProxyCameraLockMode mode)
        {
            return mode == ProxyCameraLockMode::MultiCandidate ? "multiCandidate" : "single";
        }

        ProxyUiSkyMode ParseProxyUiSkyMode(const std::string& value)
        {
            return _stricmp(value.c_str(), "classifiedProxy") == 0
                ? ProxyUiSkyMode::ClassifiedProxy
                : ProxyUiSkyMode::Off;
        }

        const char* ProxyUiSkyModeName(ProxyUiSkyMode mode)
        {
            return mode == ProxyUiSkyMode::ClassifiedProxy ? "classifiedProxy" : "off";
        }

        bool IsRuntimeDiagnosticSubmission(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::RuntimeFixedFunction ||
                   mode == BridgeSubmissionMode::RuntimeMirrorAfterOriginal;
        }

        bool IsAfterOriginalProxySubmission(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::ProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::CaptureProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::VisibleProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::RuntimeMirrorAfterOriginal;
        }

        bool IsVisibleProxySubmission(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::VisibleProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::RuntimeMirrorAfterOriginal;
        }

        bool ProxySubmissionUsesColorWrites(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::CaptureProxyAfterOriginal ||
                   IsVisibleProxySubmission(mode);
        }

        bool IsShaderDerivedTransformMode(RtxTransformMode mode)
        {
            return mode == RtxTransformMode::ShaderConstantsAuto ||
                   mode == RtxTransformMode::ShaderConstantsRegister ||
                   mode == RtxTransformMode::ShaderConstantsWvp ||
                   mode == RtxTransformMode::RatatouilleShaderConstants;
        }

        bool AllowsSanitizedCaptureProxy(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::ProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::CaptureProxyAfterOriginal;
        }

        bool IsWorldMatrixTransformMode(RtxTransformMode mode)
        {
            return mode == RtxTransformMode::RendererSlot5World ||
                   mode == RtxTransformMode::RendererMatrixCache;
        }

        bool RequiresUnsafeRuntimeReplace(BridgeSubmissionMode mode)
        {
            return mode == BridgeSubmissionMode::VisibleProxyAfterOriginal ||
                   mode == BridgeSubmissionMode::RuntimeMirrorAfterOriginal ||
                   mode == BridgeSubmissionMode::RuntimeFixedFunction ||
                   mode == BridgeSubmissionMode::ReplaceVisible;
        }

        void SanitizeBridgeConfig(
            BridgeSubmissionMode requestedSubmissionMode,
            RtxTransformMode requestedTransformMode,
            ProxyAlphaTestMode requestedAlphaTestMode)
        {
            if (RequiresUnsafeRuntimeReplace(g_config.submissionMode) && !g_config.allowUnsafeRuntimeReplace) {
                LogAlways(
                    "bridge safety guard: %s requires rtxAllowUnsafeRuntimeReplace=true; forcing originalOnly",
                    SubmissionModeName(g_config.submissionMode));
                g_config.submissionMode = BridgeSubmissionMode::OriginalOnly;
            }

            if (g_config.submissionMode != BridgeSubmissionMode::OriginalOnly &&
                IsShaderDerivedTransformMode(g_config.transformMode) &&
                !g_config.allowUnsafeShaderWvp) {
                if (AllowsSanitizedCaptureProxy(g_config.submissionMode)) {
                    LogAlways(
                        "bridge safety guard: shader transformMode=%s is unsafe for %s without rtxAllowUnsafeShaderWvp=true; forcing transformMode=rendererMatrixCache",
                        TransformModeName(g_config.transformMode),
                        SubmissionModeName(g_config.submissionMode));
                    g_config.transformMode = RtxTransformMode::RendererMatrixCache;
                } else {
                    LogAlways(
                        "bridge safety guard: transformMode=%s requires rtxAllowUnsafeShaderWvp=true for proxy/runtime submission; forcing originalOnly",
                        TransformModeName(g_config.transformMode));
                    g_config.submissionMode = BridgeSubmissionMode::OriginalOnly;
                }
            }

            if (g_config.proxyAlphaTestMode == ProxyAlphaTestMode::SolidWorldMasked &&
                g_config.transformMode != RtxTransformMode::RendererMatrixCache &&
                !g_config.allowUnsafeShaderWvp) {
                LogAlways("bridge safety guard: alpha-tested proxy capture requires rendererMatrixCache or rtxAllowUnsafeShaderWvp=true; forcing rtxProxyAlphaTestMode=skip");
                g_config.proxyAlphaTestMode = ProxyAlphaTestMode::Skip;
            }

            LogAlways(
                "bridge safety effective requested submission=%s transform=%s alphaTest=%s effective submission=%s transform=%s alphaTest=%s",
                SubmissionModeName(requestedSubmissionMode),
                TransformModeName(requestedTransformMode),
                ProxyAlphaTestModeName(requestedAlphaTestMode),
                SubmissionModeName(g_config.submissionMode),
                TransformModeName(g_config.transformMode),
                ProxyAlphaTestModeName(g_config.proxyAlphaTestMode));
        }

        bool UsesDiagnosticFixedFunctionMaterial(bool proxyOnly, bool visibleProxy)
        {
            return proxyOnly || visibleProxy || IsRuntimeDiagnosticSubmission(g_config.submissionMode);
        }

        bool ShouldPreserveFixedFunctionAlphaTest(bool proxyOnly)
        {
            return !proxyOnly || g_config.proxyAlphaTestMode != ProxyAlphaTestMode::Skip;
        }

        int ParseWvpRegister(const std::string& value)
        {
            if (_stricmp(value.c_str(), "auto") == 0 || value.empty()) {
                return -1;
            }

            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (!end || *end != '\0' || parsed < 0 || parsed > static_cast<long>(SHADER_CONSTANT_REGISTER_COUNT - 4)) {
                return -1;
            }

            return static_cast<int>(parsed);
        }

        D3DMATRIX IdentityMatrix()
        {
            D3DMATRIX matrix{};
            matrix._11 = 1.0f;
            matrix._22 = 1.0f;
            matrix._33 = 1.0f;
            matrix._44 = 1.0f;
            return matrix;
        }

        D3DMATRIX TransposeMatrix(const D3DMATRIX& matrix)
        {
            D3DMATRIX result{};
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    result.m[row][column] = matrix.m[column][row];
                }
            }

            return result;
        }

        D3DMATRIX MultiplyMatrix(const D3DMATRIX& lhs, const D3DMATRIX& rhs)
        {
            D3DMATRIX result{};
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    result.m[row][column] =
                        lhs.m[row][0] * rhs.m[0][column] +
                        lhs.m[row][1] * rhs.m[1][column] +
                        lhs.m[row][2] * rhs.m[2][column] +
                        lhs.m[row][3] * rhs.m[3][column];
                }
            }

            return result;
        }

        bool IsFiniteFloat(float value)
        {
            return std::isfinite(value) != 0;
        }

        float AbsFloat(float value)
        {
            return value < 0.0f ? -value : value;
        }

        float MinFloat(float lhs, float rhs)
        {
            return lhs < rhs ? lhs : rhs;
        }

        float MaxFloat(float lhs, float rhs)
        {
            return lhs > rhs ? lhs : rhs;
        }

        bool MatrixNearlyEqual(const D3DMATRIX& lhs, const D3DMATRIX& rhs, float tolerance)
        {
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    if (AbsFloat(lhs.m[row][column] - rhs.m[row][column]) > tolerance) {
                        return false;
                    }
                }
            }

            return true;
        }

        float Dot3(float ax, float ay, float az, float bx, float by, float bz)
        {
            return ax * bx + ay * by + az * bz;
        }

        float Length3(float x, float y, float z)
        {
            return std::sqrt(Dot3(x, y, z, x, y, z));
        }

        float Length2(float x, float y)
        {
            return std::sqrt(x * x + y * y);
        }

        float ClampFloat(float value, float minValue, float maxValue)
        {
            if (!IsFiniteFloat(value)) {
                return minValue;
            }

            if (value < minValue) {
                return minValue;
            }

            if (value > maxValue) {
                return maxValue;
            }

            return value;
        }

        bool Normalize3(float& x, float& y, float& z)
        {
            const float length = Length3(x, y, z);
            if (!IsFiniteFloat(length) || length < 1.0e-6f) {
                return false;
            }

            const float invLength = 1.0f / length;
            x *= invLength;
            y *= invLength;
            z *= invLength;
            return IsFiniteFloat(x) && IsFiniteFloat(y) && IsFiniteFloat(z);
        }

        void Cross3(
            float ax,
            float ay,
            float az,
            float bx,
            float by,
            float bz,
            float& outX,
            float& outY,
            float& outZ)
        {
            outX = ay * bz - az * by;
            outY = az * bx - ax * bz;
            outZ = ax * by - ay * bx;
        }

        BYTE ColorByte(float value)
        {
            return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        DWORD ColorToDword(float r, float g, float b, float a = 1.0f)
        {
            return D3DCOLOR_ARGB(ColorByte(a), ColorByte(r), ColorByte(g), ColorByte(b));
        }

        float Determinant3x3(const D3DMATRIX& matrix)
        {
            return matrix._11 * (matrix._22 * matrix._33 - matrix._23 * matrix._32) -
                   matrix._12 * (matrix._21 * matrix._33 - matrix._23 * matrix._31) +
                   matrix._13 * (matrix._21 * matrix._32 - matrix._22 * matrix._31);
        }

        void SetMatrixReason(char* reason, std::size_t reasonSize, const char* fmt, ...)
        {
            if (!reason || reasonSize == 0) {
                return;
            }

            va_list args;
            va_start(args, fmt);
            vsnprintf(reason, reasonSize, fmt, args);
            va_end(args);
        }

        void CopyText(char* destination, std::size_t destinationSize, const char* source)
        {
            if (!destination || destinationSize == 0) {
                return;
            }

            destination[0] = '\0';
            if (!source) {
                return;
            }

            std::strncpy(destination, source, destinationSize - 1);
            destination[destinationSize - 1] = '\0';
        }

        void SetPassDetail(FixedFunctionPassResult& result, const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(result.detail, sizeof(result.detail), fmt, args);
            va_end(args);
        }

        const char* FixedFunctionPassStatusName(FixedFunctionPassStatus status)
        {
            switch (status) {
                case FixedFunctionPassStatus::ConvertedDirect: return "convertedDirect";
                case FixedFunctionPassStatus::ConvertedRepacked: return "convertedRepacked";
                case FixedFunctionPassStatus::SkippedUi: return "skippedUi";
                default: return "unsupported";
            }
        }

        const char* FixedFunctionPassPathName(FixedFunctionPassPath path)
        {
            switch (path) {
                case FixedFunctionPassPath::DirectFvf: return "directFvf";
                case FixedFunctionPassPath::Repacked: return "repacked";
                default: return "none";
            }
        }

        const char* FixedFunctionPassReasonName(FixedFunctionPassReason reason)
        {
            switch (reason) {
                case FixedFunctionPassReason::ConvertedDirect: return "convertedDirect";
                case FixedFunctionPassReason::ConvertedRepacked: return "convertedRepacked";
                case FixedFunctionPassReason::SkippedPassMode: return "passMode2";
                case FixedFunctionPassReason::SkippedZDisabled: return "zDisabled";
                case FixedFunctionPassReason::SkippedLikelyUi: return "likelyUi";
                case FixedFunctionPassReason::SkippedRhw: return "rhw";
                case FixedFunctionPassReason::SkippedScreenMatrix: return "screenMatrix";
                case FixedFunctionPassReason::NoTransform: return "noTransform";
                case FixedFunctionPassReason::BadAffine: return "badAffine";
                case FixedFunctionPassReason::BadViewProjection: return "badViewProjection";
                case FixedFunctionPassReason::BadVertexBounds: return "badVertexBounds";
                case FixedFunctionPassReason::NoWvpCandidate: return "wvpNoCandidate";
                case FixedFunctionPassReason::InvalidIndexRange: return "invalidIndexRange";
                case FixedFunctionPassReason::CollapsedTransform: return "collapsedTransform";
                case FixedFunctionPassReason::DirectFvfOnlyFailed: return "directFvfOnlyFailed";
                case FixedFunctionPassReason::NonTrianglePrimitive: return "nonTrianglePrimitive";
                case FixedFunctionPassReason::RepackLayoutFailed: return "repackLayoutFailed";
                case FixedFunctionPassReason::MissingTexcoord: return "missingTexcoord";
                case FixedFunctionPassReason::SkippedSkinned: return "skinned";
                case FixedFunctionPassReason::SkippedPatch: return "patch";
                case FixedFunctionPassReason::SkippedTransparent: return "transparent";
                case FixedFunctionPassReason::SkippedScreenCopy: return "screenCopy";
                case FixedFunctionPassReason::SkippedDuplicateCommand: return "duplicateCommand";
                case FixedFunctionPassReason::SkippedCameraMismatch: return "cameraMismatch";
                case FixedFunctionPassReason::SkippedScreenPlane: return "screenPlane";
                case FixedFunctionPassReason::SkippedProjectedOversize: return "projectedOversize";
                case FixedFunctionPassReason::VertexCountRejected: return "vertexCountRejected";
                case FixedFunctionPassReason::RepackSourceUnavailable: return "repackSourceUnavailable";
                case FixedFunctionPassReason::RepackLockFailed: return "repackLockFailed";
                case FixedFunctionPassReason::StreamOverrideFailed: return "streamOverrideFailed";
                case FixedFunctionPassReason::DrawFailed: return "drawFailed";
                case FixedFunctionPassReason::Unsupported: return "unsupported";
                default: return "none";
            }
        }

        const char* ProxyDrawClassName(ProxyDrawClass value)
        {
            switch (value) {
                case ProxyDrawClass::Ui: return "ui";
                case ProxyDrawClass::Sky: return "sky";
                default: return "world";
            }
        }

        bool NearlyAbs(float value, float target, float tolerance)
        {
            return AbsFloat(AbsFloat(value) - target) <= tolerance;
        }

        bool MatrixNearlyIdentity(const D3DMATRIX& matrix, float tolerance)
        {
            const D3DMATRIX identity = IdentityMatrix();
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    if (AbsFloat(matrix.m[row][column] - identity.m[row][column]) > tolerance) {
                        return false;
                    }
                }
            }

            return true;
        }

        bool LooksLikeScreenMatrix(const D3DMATRIX& matrix, const D3DVIEWPORT9* viewport, char* reason, std::size_t reasonSize)
        {
            const float row0xy = Length2(matrix._11, matrix._12);
            const float row1xy = Length2(matrix._21, matrix._22);

            if (row0xy > 128.0f || row1xy > 128.0f) {
                SetMatrixReason(reason, reasonSize, "screen scale row0xy=%g row1xy=%g", row0xy, row1xy);
                return true;
            }

            if (!viewport || viewport->Width == 0 || viewport->Height == 0) {
                return false;
            }

            const float halfWidth = static_cast<float>(viewport->Width) * 0.5f;
            const float halfHeight = static_cast<float>(viewport->Height) * 0.5f;
            const float tolerance = 4.0f;
            const float values[] = {
                matrix._11, matrix._12, matrix._21, matrix._22,
                matrix._31, matrix._32, matrix._41, matrix._42
            };

            int viewportHits = 0;
            for (float value : values) {
                if (NearlyAbs(value, halfWidth, tolerance) || NearlyAbs(value, halfHeight, tolerance)) {
                    ++viewportHits;
                }
            }

            if (viewportHits >= 2) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "viewport constants hits=%d half=%gx%g",
                    viewportHits,
                    halfWidth,
                    halfHeight);
                return true;
            }

            return false;
        }

        bool MatrixValuesAreFinite(const D3DMATRIX& matrix, char* reason, std::size_t reasonSize)
        {
            const float* values = reinterpret_cast<const float*>(&matrix);
            for (int i = 0; i < 16; ++i) {
                if (!IsFiniteFloat(values[i])) {
                    SetMatrixReason(reason, reasonSize, "non-finite value index=%d", i);
                    return false;
                }
            }

            return true;
        }

        bool IsReadableMemoryRange(const void* address, std::size_t size)
        {
            if (!address || size == 0) {
                return false;
            }

            const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
            const std::uintptr_t end = begin + size;
            if (end < begin) {
                return false;
            }

            std::uintptr_t cursor = begin;
            while (cursor < end) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info) ||
                    info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
                    return false;
                }

                const std::uintptr_t regionEnd =
                    reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
                if (regionEnd <= cursor) {
                    return false;
                }
                cursor = regionEnd;
            }

            return true;
        }

        bool ValidateRendererViewMatrix(const D3DMATRIX& matrix, char* reason, std::size_t reasonSize)
        {
            if (!MatrixValuesAreFinite(matrix, reason, reasonSize)) {
                return false;
            }

            if (AbsFloat(matrix._14) > 1.0e-3f || AbsFloat(matrix._24) > 1.0e-3f ||
                AbsFloat(matrix._34) > 1.0e-3f || AbsFloat(matrix._44 - 1.0f) > 1.0e-2f) {
                SetMatrixReason(reason, reasonSize, "view is not affine column=(%g,%g,%g,%g)",
                    matrix._14, matrix._24, matrix._34, matrix._44);
                return false;
            }

            const float row0 = Length3(matrix._11, matrix._12, matrix._13);
            const float row1 = Length3(matrix._21, matrix._22, matrix._23);
            const float row2 = Length3(matrix._31, matrix._32, matrix._33);
            if (row0 < 0.25f || row1 < 0.25f || row2 < 0.25f ||
                row0 > 4.0f || row1 > 4.0f || row2 > 4.0f) {
                SetMatrixReason(reason, reasonSize, "view basis lengths rejected=%g,%g,%g", row0, row1, row2);
                return false;
            }

            const float dot01 = AbsFloat(Dot3(matrix._11, matrix._12, matrix._13, matrix._21, matrix._22, matrix._23)) / (row0 * row1);
            const float dot02 = AbsFloat(Dot3(matrix._11, matrix._12, matrix._13, matrix._31, matrix._32, matrix._33)) / (row0 * row2);
            const float dot12 = AbsFloat(Dot3(matrix._21, matrix._22, matrix._23, matrix._31, matrix._32, matrix._33)) / (row1 * row2);
            if (dot01 > 0.25f || dot02 > 0.25f || dot12 > 0.25f) {
                SetMatrixReason(reason, reasonSize, "view basis is not orthogonal dots=%g,%g,%g", dot01, dot02, dot12);
                return false;
            }

            SetMatrixReason(reason, reasonSize, "view affine ok basis=%g,%g,%g", row0, row1, row2);
            return true;
        }

        bool ValidateRendererProjectionMatrix(
            const D3DMATRIX& matrix,
            bool& orthographic,
            char* reason,
            std::size_t reasonSize)
        {
            orthographic = false;
            if (!MatrixValuesAreFinite(matrix, reason, reasonSize) || MatrixNearlyIdentity(matrix, 1.0e-4f)) {
                if (!reason[0]) {
                    SetMatrixReason(reason, reasonSize, "projection is identity");
                }
                return false;
            }

            const float scaleX = AbsFloat(matrix._11);
            const float scaleY = AbsFloat(matrix._22);
            const float depthScale = AbsFloat(matrix._33);
            if (scaleX < 1.0e-6f || scaleY < 1.0e-6f || depthScale < 1.0e-6f ||
                scaleX > 1.0e5f || scaleY > 1.0e5f || depthScale > 1.0e5f) {
                SetMatrixReason(reason, reasonSize, "projection scales rejected=%g,%g,%g", scaleX, scaleY, depthScale);
                return false;
            }

            const bool perspective = AbsFloat(matrix._34) > 0.5f &&
                AbsFloat(matrix._44) < 0.05f && AbsFloat(matrix._43) > 1.0e-6f;
            orthographic = AbsFloat(matrix._34) < 0.05f &&
                AbsFloat(matrix._44 - 1.0f) < 0.05f;
            if (!perspective && !orthographic) {
                SetMatrixReason(reason, reasonSize, "projection shape rejected _34=%g _43=%g _44=%g",
                    matrix._34, matrix._43, matrix._44);
                return false;
            }

            SetMatrixReason(reason, reasonSize, "%s projection ok scale=%g,%g depth=%g",
                orthographic ? "orthographic" : "perspective", scaleX, scaleY, depthScale);
            return true;
        }

        bool LooksLikeShiftedAffineWindow(const D3DMATRIX& matrix, char* reason, std::size_t reasonSize)
        {
            const float bottomXyz = Length3(matrix._41, matrix._42, matrix._43);
            if (bottomXyz < 1.0e-4f && AbsFloat(matrix._44 - 1.0f) < 1.0e-3f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "column/affine-like WVP window bottom=[%g,%g,%g,%g]",
                    matrix._41,
                    matrix._42,
                    matrix._43,
                    matrix._44);
                return true;
            }

            const float rows[3][3] = {
                { matrix._11, matrix._12, matrix._13 },
                { matrix._21, matrix._22, matrix._23 },
                { matrix._31, matrix._32, matrix._33 },
            };

            for (int lhs = 0; lhs < 3; ++lhs) {
                const float lhsLen = Length3(rows[lhs][0], rows[lhs][1], rows[lhs][2]);
                if (lhsLen < 1.0e-5f) {
                    continue;
                }

                for (int rhs = lhs + 1; rhs < 3; ++rhs) {
                    const float rhsLen = Length3(rows[rhs][0], rows[rhs][1], rows[rhs][2]);
                    if (rhsLen < 1.0e-5f) {
                        continue;
                    }

                    const float dot =
                        AbsFloat(Dot3(
                            rows[lhs][0],
                            rows[lhs][1],
                            rows[lhs][2],
                            rows[rhs][0],
                            rows[rhs][1],
                            rows[rhs][2])) /
                        (lhsLen * rhsLen);
                    const float lenRatio = MinFloat(lhsLen, rhsLen) / MaxFloat(lhsLen, rhsLen);
                    if (dot > 0.999f && lenRatio > 0.98f) {
                        SetMatrixReason(
                            reason,
                            reasonSize,
                            "shifted affine/view WVP rows %d/%d parallel dot=%g lenRatio=%g",
                            lhs,
                            rhs,
                            dot,
                            lenRatio);
                        return true;
                    }
                }
            }

            return false;
        }

        bool ValidateAffineWorldMatrix(const D3DMATRIX& matrix, const D3DVIEWPORT9* viewport, char* reason, std::size_t reasonSize)
        {
            if (!MatrixValuesAreFinite(matrix, reason, reasonSize)) {
                return false;
            }

            if (LooksLikeScreenMatrix(matrix, viewport, reason, reasonSize)) {
                return false;
            }

            if (AbsFloat(matrix._14) > 1.0e-3f || AbsFloat(matrix._24) > 1.0e-3f ||
                AbsFloat(matrix._34) > 1.0e-3f || AbsFloat(matrix._44 - 1.0f) > 1.0e-2f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "not affine column=(%g,%g,%g,%g)",
                    matrix._14,
                    matrix._24,
                    matrix._34,
                    matrix._44);
                return false;
            }

            const float row0 = Length3(matrix._11, matrix._12, matrix._13);
            const float row1 = Length3(matrix._21, matrix._22, matrix._23);
            const float row2 = Length3(matrix._31, matrix._32, matrix._33);
            if (row0 < 1.0e-4f || row1 < 1.0e-4f || row2 < 1.0e-4f ||
                row0 > 1000.0f || row1 > 1000.0f || row2 > 1000.0f) {
                SetMatrixReason(reason, reasonSize, "bad affine basis lengths=%g,%g,%g", row0, row1, row2);
                return false;
            }

            const float maxTranslation = MaxFloat(AbsFloat(matrix._41), MaxFloat(AbsFloat(matrix._42), AbsFloat(matrix._43)));
            if (maxTranslation > 1.0e6f) {
                SetMatrixReason(reason, reasonSize, "bad affine translation max=%g", maxTranslation);
                return false;
            }

            SetMatrixReason(reason, reasonSize, "affine ok basis=%g,%g,%g translation=%g", row0, row1, row2, maxTranslation);
            return true;
        }

        bool ValidateDeviceViewProjection(IDirect3DDevice9* device, char* reason, std::size_t reasonSize)
        {
            if (!device) {
                SetMatrixReason(reason, reasonSize, "device is null");
                return false;
            }

            D3DMATRIX view{};
            D3DMATRIX projection{};
            if (FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) {
                SetMatrixReason(reason, reasonSize, "GetTransform VIEW/PROJECTION failed");
                return false;
            }

            char viewReason[96]{};
            char projectionReason[96]{};
            if (!MatrixValuesAreFinite(view, viewReason, sizeof(viewReason)) ||
                !MatrixValuesAreFinite(projection, projectionReason, sizeof(projectionReason))) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "non-finite view/projection view=%s projection=%s",
                    viewReason[0] ? viewReason : "ok",
                    projectionReason[0] ? projectionReason : "ok");
                return false;
            }

            const bool viewIdentity = MatrixNearlyIdentity(view, 1.0e-4f);
            const bool projectionIdentity = MatrixNearlyIdentity(projection, 1.0e-4f);
            if (viewIdentity || projectionIdentity) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "identity view/projection rejected viewIdentity=%d projectionIdentity=%d",
                    viewIdentity ? 1 : 0,
                    projectionIdentity ? 1 : 0);
                return false;
            }

            const float viewBasis =
                Length3(view._11, view._12, view._13) +
                Length3(view._21, view._22, view._23) +
                Length3(view._31, view._32, view._33);
            const float projectionSum =
                AbsFloat(projection._11) + AbsFloat(projection._22) +
                AbsFloat(projection._33) + AbsFloat(projection._34) +
                AbsFloat(projection._43) + AbsFloat(projection._44);
            if (viewBasis < 1.0e-4f || projectionSum < 1.0e-4f) {
                SetMatrixReason(reason, reasonSize, "empty view/projection basis=%g projectionSum=%g", viewBasis, projectionSum);
                return false;
            }

            SetMatrixReason(reason, reasonSize, "view/projection ok");
            return true;
        }

        bool TransformPointRowMajorFull(
            const D3DMATRIX& matrix,
            float x,
            float y,
            float z,
            float& outX,
            float& outY,
            float& outZ,
            float& outW)
        {
            const float clipX = x * matrix._11 + y * matrix._21 + z * matrix._31 + matrix._41;
            const float clipY = x * matrix._12 + y * matrix._22 + z * matrix._32 + matrix._42;
            const float clipZ = x * matrix._13 + y * matrix._23 + z * matrix._33 + matrix._43;
            const float clipW = x * matrix._14 + y * matrix._24 + z * matrix._34 + matrix._44;
            outX = clipX;
            outY = clipY;
            outZ = clipZ;
            outW = clipW;

            return IsFiniteFloat(clipX) && IsFiniteFloat(clipY) && IsFiniteFloat(clipZ) &&
                   IsFiniteFloat(clipW) && AbsFloat(clipW) > 1.0e-6f;
        }

        bool TransformPointRowMajor(const D3DMATRIX& matrix, float x, float y, float z, float& outW)
        {
            float clipX = 0.0f;
            float clipY = 0.0f;
            float clipZ = 0.0f;
            return TransformPointRowMajorFull(matrix, x, y, z, clipX, clipY, clipZ, outW);
        }

        float ScoreMatrixCandidate(
            const D3DMATRIX& matrix,
            const D3DVIEWPORT9* viewport,
            bool& screenRejected,
            char* reason,
            std::size_t reasonSize)
        {
            screenRejected = false;
            const float* values = reinterpret_cast<const float*>(&matrix);
            float maxAbs = 0.0f;
            float sumAbs = 0.0f;

            for (int i = 0; i < 16; ++i) {
                if (!IsFiniteFloat(values[i])) {
                    SetMatrixReason(reason, reasonSize, "non-finite value index=%d", i);
                    return -1.0f;
                }

                const float absValue = AbsFloat(values[i]);
                if (absValue > maxAbs) {
                    maxAbs = absValue;
                }
                sumAbs += absValue;
            }

            if (maxAbs < 1.0e-5f || maxAbs > 1.0e6f || sumAbs < 1.0e-4f) {
                SetMatrixReason(reason, reasonSize, "invalid magnitude max=%g sum=%g", maxAbs, sumAbs);
                return -1.0f;
            }

            if (LooksLikeScreenMatrix(matrix, viewport, reason, reasonSize)) {
                screenRejected = true;
                return -1.0f;
            }

            float score = 1.0f;
            const float row0 = Length3(matrix._11, matrix._12, matrix._13);
            const float row1 = Length3(matrix._21, matrix._22, matrix._23);
            const float row2 = Length3(matrix._31, matrix._32, matrix._33);
            const float det = AbsFloat(Determinant3x3(matrix));

            if (row0 <= 1.0e-5f || row1 <= 1.0e-5f || row2 <= 1.0e-5f ||
                row0 > 256.0f || row1 > 256.0f || row2 > 256.0f) {
                SetMatrixReason(reason, reasonSize, "row scale rejected rows=%g,%g,%g", row0, row1, row2);
                return -1.0f;
            }

            score += 3.0f;
            if (det > 1.0e-8f && det < 1.0e8f) score += 1.5f;

            float w = 0.0f;
            const float points[][3] = {
                { 0.0f, 0.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f }
            };

            for (const auto& point : points) {
                if (TransformPointRowMajor(matrix, point[0], point[1], point[2], w) && AbsFloat(w) < 1.0e6f) {
                    score += 0.75f;
                }
            }

            return score;
        }

        void LogRejectedScreenMatrix(
            const D3DMATRIX& matrix,
            int startRegister,
            const char* source,
            bool transposed,
            const char* reason);

        float ScoreWvpCandidateAgainstBounds(
            const D3DMATRIX& matrix,
            const D3DVIEWPORT9* viewport,
            const VertexBounds& bounds,
            bool& screenRejected,
            char* reason,
            std::size_t reasonSize)
        {
            screenRejected = false;
            if (!bounds.valid) {
                SetMatrixReason(reason, reasonSize, "bounds invalid");
                return -1.0f;
            }

            if (!MatrixValuesAreFinite(matrix, reason, reasonSize)) {
                return -1.0f;
            }

            if (LooksLikeScreenMatrix(matrix, viewport, reason, reasonSize)) {
                screenRejected = true;
                return -1.0f;
            }

            const float* values = reinterpret_cast<const float*>(&matrix);
            float maxMatrixAbs = 0.0f;
            float sumMatrixAbs = 0.0f;
            for (int i = 0; i < 16; ++i) {
                const float absValue = AbsFloat(values[i]);
                maxMatrixAbs = MaxFloat(maxMatrixAbs, absValue);
                sumMatrixAbs += absValue;
            }

            if (maxMatrixAbs < 1.0e-6f || maxMatrixAbs > 1.0e7f || sumMatrixAbs < 1.0e-5f) {
                SetMatrixReason(reason, reasonSize, "invalid matrix magnitude max=%g sum=%g", maxMatrixAbs, sumMatrixAbs);
                return -1.0f;
            }

            const float row0 = Length3(matrix._11, matrix._12, matrix._13);
            const float row1 = Length3(matrix._21, matrix._22, matrix._23);
            const float row2 = Length3(matrix._31, matrix._32, matrix._33);
            if (row0 < 1.0e-5f || row1 < 1.0e-5f || row2 < 1.0e-5f) {
                SetMatrixReason(reason, reasonSize, "zero matrix row lengths=%g,%g,%g", row0, row1, row2);
                return -1.0f;
            }

            if (LooksLikeShiftedAffineWindow(matrix, reason, reasonSize)) {
                return -1.0f;
            }

            const float perspectiveLen = Length3(matrix._14, matrix._24, matrix._34);
            if (perspectiveLen < 1.0e-6f && AbsFloat(matrix._44 - 1.0f) < 1.0e-3f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "affine-like candidate rejected perspectiveLen=%g w44=%g",
                    perspectiveLen,
                    matrix._44);
                return -1.0f;
            }

            const float corners[8][3] = {
                { bounds.minX, bounds.minY, bounds.minZ },
                { bounds.maxX, bounds.minY, bounds.minZ },
                { bounds.minX, bounds.maxY, bounds.minZ },
                { bounds.maxX, bounds.maxY, bounds.minZ },
                { bounds.minX, bounds.minY, bounds.maxZ },
                { bounds.maxX, bounds.minY, bounds.maxZ },
                { bounds.minX, bounds.maxY, bounds.maxZ },
                { bounds.maxX, bounds.maxY, bounds.maxZ },
            };

            int finiteCorners = 0;
            int looseClipCorners = 0;
            int wideClipCorners = 0;
            float maxAbsNdc = 0.0f;
            float minNdcX = 1.0e30f;
            float minNdcY = 1.0e30f;
            float maxNdcX = -1.0e30f;
            float maxNdcY = -1.0e30f;
            float minAbsW = 1.0e30f;
            float maxAbsW = 0.0f;
            float minClipW = 1.0e30f;
            float maxClipW = -1.0e30f;

            for (const auto& corner : corners) {
                float clipX = 0.0f;
                float clipY = 0.0f;
                float clipZ = 0.0f;
                float clipW = 0.0f;
                if (!TransformPointRowMajorFull(matrix, corner[0], corner[1], corner[2], clipX, clipY, clipZ, clipW)) {
                    continue;
                }

                const float invW = 1.0f / clipW;
                const float ndcX = clipX * invW;
                const float ndcY = clipY * invW;
                const float ndcZ = clipZ * invW;
                if (!IsFiniteFloat(ndcX) || !IsFiniteFloat(ndcY) || !IsFiniteFloat(ndcZ)) {
                    continue;
                }

                const float absW = AbsFloat(clipW);
                minAbsW = MinFloat(minAbsW, absW);
                maxAbsW = MaxFloat(maxAbsW, absW);
                minClipW = MinFloat(minClipW, clipW);
                maxClipW = MaxFloat(maxClipW, clipW);
                maxAbsNdc = MaxFloat(maxAbsNdc, MaxFloat(MaxFloat(AbsFloat(ndcX), AbsFloat(ndcY)), AbsFloat(ndcZ)));
                minNdcX = MinFloat(minNdcX, ndcX);
                minNdcY = MinFloat(minNdcY, ndcY);
                maxNdcX = MaxFloat(maxNdcX, ndcX);
                maxNdcY = MaxFloat(maxNdcY, ndcY);
                ++finiteCorners;

                if (AbsFloat(ndcX) <= 2.5f && AbsFloat(ndcY) <= 2.5f && ndcZ >= -2.0f && ndcZ <= 2.5f) {
                    ++looseClipCorners;
                }

                if (AbsFloat(ndcX) <= 16.0f && AbsFloat(ndcY) <= 16.0f && ndcZ >= -16.0f && ndcZ <= 16.0f) {
                    ++wideClipCorners;
                }
            }

            if (finiteCorners < 4) {
                SetMatrixReason(reason, reasonSize, "too few finite clipped bounds corners finite=%d", finiteCorners);
                return -1.0f;
            }

            if (wideClipCorners == 0 || maxAbsNdc > 256.0f || maxAbsW > 1.0e8f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "bounds outside clip wide=%d maxNdc=%g wRange=%g..%g",
                    wideClipCorners,
                    maxAbsNdc,
                    minAbsW,
                    maxAbsW);
                return -1.0f;
            }

            const float clipWRange = maxClipW - minClipW;
            const float clipWScale = MaxFloat(AbsFloat(minClipW), AbsFloat(maxClipW));
            const float clipWRelativeRange = clipWScale > 1.0e-4f ? clipWRange / clipWScale : clipWRange;
            if (!IsFiniteFloat(clipWRange) || clipWRange < 1.0e-4f || clipWRelativeRange < 1.0e-4f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "insufficient clip W variation w=%g..%g rel=%g",
                    minClipW,
                    maxClipW,
                    clipWRelativeRange);
                return -1.0f;
            }

            const float ndcExtentX = maxNdcX - minNdcX;
            const float ndcExtentY = maxNdcY - minNdcY;
            const float ndcArea = ndcExtentX * ndcExtentY;
            if (!IsFiniteFloat(ndcArea) || ndcExtentX < 1.0e-5f || ndcExtentY < 1.0e-5f || ndcArea < 1.0e-8f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "collapsed projected bounds extent=%gx%g area=%g maxNdc=%g",
                    ndcExtentX,
                    ndcExtentY,
                    ndcArea,
                    maxAbsNdc);
                return -1.0f;
            }

            float score =
                4.0f +
                static_cast<float>(finiteCorners) +
                static_cast<float>(wideClipCorners) * 2.0f +
                static_cast<float>(looseClipCorners) * 8.0f;
            if (perspectiveLen >= 1.0e-5f) {
                score += 4.0f;
            }
            if (minAbsW > 1.0e-4f && maxAbsW < 1.0e6f) {
                score += 2.0f;
            }
            score -= MinFloat(maxAbsNdc, 256.0f) * 0.01f;

            SetMatrixReason(
                reason,
                reasonSize,
                "bounds score finite=%d loose=%d wide=%d ndcExtent=%gx%g maxNdc=%g wRange=%g..%g persp=%g",
                finiteCorners,
                looseClipCorners,
                wideClipCorners,
                ndcExtentX,
                ndcExtentY,
                maxAbsNdc,
                minAbsW,
                maxAbsW,
                perspectiveLen);
            return score;
        }

        MatrixSelection EvaluateWvpMatrixCandidate(
            const D3DMATRIX& matrix,
            int startRegister,
            const char* source,
            const D3DVIEWPORT9* viewport,
            const VertexBounds& bounds)
        {
            MatrixSelection best{};
            CopyText(best.source, sizeof(best.source), source);
            best.startRegister = startRegister;

            auto consider = [&](const D3DMATRIX& candidate, bool transposed) {
                bool screenRejected = false;
                char reason[160]{};
                const float score = ScoreWvpCandidateAgainstBounds(candidate, viewport, bounds, screenRejected, reason, sizeof(reason));
                ++g_counters.matrixCandidates;
                ++g_counters.shaderWvpCandidates;
                if (screenRejected) {
                    best.screenRejected = true;
                    CopyText(best.reason, sizeof(best.reason), reason);
                    LogRejectedScreenMatrix(candidate, startRegister, source, transposed, reason);
                }
                if (score <= 0.0f) {
                    if (!best.reason[0]) {
                        CopyText(best.reason, sizeof(best.reason), reason);
                    }
                    return;
                }

                if (!best.ok || score > best.score) {
                    best.ok = true;
                    best.matrix = candidate;
                    best.transposed = transposed;
                    best.score = score;
                    CopyText(best.reason, sizeof(best.reason), reason);
                }
            };

            if (g_config.wvpTransposeMode != WvpTransposeMode::ForceTrue) {
                consider(matrix, false);
            }
            if (g_config.wvpTransposeMode != WvpTransposeMode::ForceFalse) {
                consider(TransposeMatrix(matrix), true);
            }

            if (!best.ok && !best.reason[0]) {
                snprintf(best.reason, sizeof(best.reason), "no bounds-valid WVP candidate source=%s register=%d", source, startRegister);
            }

            return best;
        }

        bool IsRhwFvf(DWORD fvf)
        {
            constexpr DWORD positionMask = 0x400E;
            return (fvf & positionMask) == D3DFVF_XYZRHW;
        }

        std::uint32_t HashMatrixSelection(const MatrixSelection& selection)
        {
            std::uint32_t hash = 2166136261u;
            HashValue(hash, selection.startRegister);
            HashValue(hash, selection.transposed);
            HashValue(hash, selection.score);
            const float* values = reinterpret_cast<const float*>(&selection.matrix);
            for (int i = 0; i < 16; ++i) {
                HashValue(hash, values[i]);
            }

            return hash;
        }

        void LogMatrixSelection(const MatrixSelection& selection)
        {
            if (!selection.ok || (!g_config.logFile && !g_config.debugLog)) {
                return;
            }

            const std::uint32_t hash = HashMatrixSelection(selection);
            for (std::size_t i = 0; i < g_loggedMatrixCount; ++i) {
                if (g_loggedMatrixHashes[i] == hash) {
                    return;
                }
            }

            if (g_loggedMatrixCount >= g_loggedMatrixHashes.size()) {
                return;
            }

            g_loggedMatrixHashes[g_loggedMatrixCount++] = hash;
            LogAlways(
                "wvp source=%s register=%d transposed=%d score=%.3f reason=\"%s\" rows=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                selection.source,
                selection.startRegister,
                selection.transposed ? 1 : 0,
                selection.score,
                selection.reason[0] ? selection.reason : "(none)",
                selection.matrix._11,
                selection.matrix._12,
                selection.matrix._13,
                selection.matrix._14,
                selection.matrix._21,
                selection.matrix._22,
                selection.matrix._23,
                selection.matrix._24,
                selection.matrix._31,
                selection.matrix._32,
                selection.matrix._33,
                selection.matrix._34,
                selection.matrix._41,
                selection.matrix._42,
                selection.matrix._43,
                selection.matrix._44);
        }

        void LogDrawWvpSelection(const char* prefix, const PrimitiveDraw* draw, const VertexBounds& bounds, const MatrixSelection& selection)
        {
            if (!selection.ok || (!g_config.logFile && !g_config.debugLog) || !draw) {
                return;
            }

            static std::uint32_t logged = 0;
            if (logged >= 48) {
                return;
            }

            ++logged;
            LogAlways(
                "%s WVP selected stride=%u primitive=%u indexed=%d register=%d transposed=%d score=%.3f source=%s reason=\"%s\" bounds=[%g,%g,%g]-[%g,%g,%g]",
                prefix && prefix[0] ? prefix : "draw",
                draw->stride,
                draw->primitiveType,
                draw->indexBuffer ? 1 : 0,
                selection.startRegister,
                selection.transposed ? 1 : 0,
                selection.score,
                selection.source,
                selection.reason[0] ? selection.reason : "(none)",
                bounds.minX,
                bounds.minY,
                bounds.minZ,
                bounds.maxX,
                bounds.maxY,
                bounds.maxZ);
        }

        void LogLiveShaderWvpReject(int reg, const MatrixSelection& candidate)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            if (g_counters.shaderWvpRejectedLogs >= 64) {
                return;
            }

            ++g_counters.shaderWvpRejectedLogs;
            LogAlways(
                "live shader WVP reject register=%d transposed=%d screenRejected=%d reason=\"%s\"",
                reg,
                candidate.transposed ? 1 : 0,
                candidate.screenRejected ? 1 : 0,
                candidate.reason[0] ? candidate.reason : "unknown");
        }

        void RecordMatrixProbeCandidate(const char* source, const D3DMATRIX& matrix, std::uintptr_t caller)
        {
            char reason[96]{};
            if (!MatrixValuesAreFinite(matrix, reason, sizeof(reason)) ||
                MatrixNearlyIdentity(matrix, 1.0e-4f)) {
                return;
            }

            MatrixProbeCandidate& candidate = g_matrixProbeCandidates[g_matrixProbeCursor++ % g_matrixProbeCandidates.size()];
            candidate.valid = true;
            candidate.matrix = matrix;
            candidate.caller = caller;
            candidate.sequence = ++g_matrixProbeSequence;
            CopyText(candidate.source, sizeof(candidate.source), source && source[0] ? source : "matrixProbe");
            ++g_counters.matrixProbeCaptured;

            if ((g_config.logFile || g_config.debugLog) && g_counters.matrixProbeCaptured <= 24) {
                LogAlways(
                    "matrix probe captured source=%s caller=0x%p seq=%llu rows=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                    candidate.source,
                    reinterpret_cast<void*>(caller),
                    static_cast<unsigned long long>(candidate.sequence),
                    matrix._11,
                    matrix._12,
                    matrix._13,
                    matrix._14,
                    matrix._21,
                    matrix._22,
                    matrix._23,
                    matrix._24,
                    matrix._31,
                    matrix._32,
                    matrix._33,
                    matrix._34,
                    matrix._41,
                    matrix._42,
                    matrix._43,
                    matrix._44);
            }
        }

        void LogRejectedScreenMatrix(
            const D3DMATRIX& matrix,
            int startRegister,
            const char* source,
            bool transposed,
            const char* reason)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            static std::uint32_t loggedRejects = 0;
            if (loggedRejects >= 16) {
                return;
            }

            ++loggedRejects;
            LogAlways(
                "wvp rejected screen source=%s register=%d transposed=%d reason=\"%s\" rows=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                source,
                startRegister,
                transposed ? 1 : 0,
                reason && reason[0] ? reason : "(none)",
                matrix._11,
                matrix._12,
                matrix._13,
                matrix._14,
                matrix._21,
                matrix._22,
                matrix._23,
                matrix._24,
                matrix._31,
                matrix._32,
                matrix._33,
                matrix._34,
                matrix._41,
                matrix._42,
                matrix._43,
                matrix._44);
        }

        MatrixSelection EvaluateMatrixCandidate(
            const D3DMATRIX& matrix,
            int startRegister,
            const char* source,
            const D3DVIEWPORT9* viewport)
        {
            MatrixSelection best{};
            std::strncpy(best.source, source, sizeof(best.source) - 1);
            best.startRegister = startRegister;

            auto consider = [&](const D3DMATRIX& candidate, bool transposed) {
                bool screenRejected = false;
                char reason[160]{};
                const float score = ScoreMatrixCandidate(candidate, viewport, screenRejected, reason, sizeof(reason));
                ++g_counters.matrixCandidates;
                if (screenRejected) {
                    best.screenRejected = true;
                    std::strncpy(best.reason, reason, sizeof(best.reason) - 1);
                    LogRejectedScreenMatrix(candidate, startRegister, source, transposed, reason);
                }
                if (score <= 0.0f) {
                    return;
                }

                if (!best.ok || score > best.score) {
                    best.ok = true;
                    best.matrix = candidate;
                    best.transposed = transposed;
                    best.score = score;
                }
            };

            if (g_config.wvpTransposeMode != WvpTransposeMode::ForceTrue) {
                consider(matrix, false);
            }
            if (g_config.wvpTransposeMode != WvpTransposeMode::ForceFalse) {
                consider(TransposeMatrix(matrix), true);
            }

            if (!best.ok) {
                if (!best.reason[0]) {
                    snprintf(best.reason, sizeof(best.reason), "no sane matrix candidate source=%s register=%d", source, startRegister);
                }
            }

            return best;
        }

        bool SelectShaderConstantMatrix(IDirect3DDevice9* device, MatrixSelection& selection)
        {
            std::array<float, SHADER_CONSTANT_REGISTER_COUNT * 4> constants{};
            const HRESULT hr = device->GetVertexShaderConstantF(0, constants.data(), SHADER_CONSTANT_REGISTER_COUNT);
            if (FAILED(hr)) {
                snprintf(selection.reason, sizeof(selection.reason), "GetVertexShaderConstantF failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            const int startRegister = g_config.transformMode == RtxTransformMode::ShaderConstantsRegister
                ? g_config.wvpRegister
                : -1;

            if (startRegister >= 0) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, constants.data() + startRegister * 4, sizeof(matrix));
                selection = EvaluateMatrixCandidate(matrix, startRegister, "shaderConstants", viewportPtr);
                return selection.ok;
            }

            if (g_config.autoLockWvpRegister && g_lockedWvpValid) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, constants.data() + g_lockedWvpRegister * 4, sizeof(matrix));
                if (g_lockedWvpTransposed) {
                    matrix = TransposeMatrix(matrix);
                }

                bool screenRejected = false;
                char reason[160]{};
                const float score = ScoreMatrixCandidate(matrix, viewportPtr, screenRejected, reason, sizeof(reason));
                ++g_counters.matrixCandidates;
                if (score > 0.0f) {
                    selection.ok = true;
                    selection.matrix = matrix;
                    selection.startRegister = g_lockedWvpRegister;
                    selection.transposed = g_lockedWvpTransposed;
                    selection.score = score;
                    std::strncpy(selection.source, "shaderConstants", sizeof(selection.source) - 1);
                    return true;
                }

                selection.screenRejected = screenRejected;
                if (screenRejected) {
                    LogRejectedScreenMatrix(matrix, g_lockedWvpRegister, "shaderConstantsLocked", g_lockedWvpTransposed, reason);
                }
                snprintf(
                    selection.reason,
                    sizeof(selection.reason),
                    "locked shader constant matrix rejected register=%d transposed=%d reason=%s",
                    g_lockedWvpRegister,
                    g_lockedWvpTransposed ? 1 : 0,
                    reason[0] ? reason : "unknown");
                return false;
            }

            MatrixSelection best{};
            bool sawScreenRejected = false;
            char firstScreenReason[160]{};
            for (int reg = 0; reg <= static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT - 4); ++reg) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, constants.data() + reg * 4, sizeof(matrix));
                MatrixSelection candidate = EvaluateMatrixCandidate(matrix, reg, "shaderConstants", viewportPtr);
                if (candidate.screenRejected) {
                    sawScreenRejected = true;
                    if (!firstScreenReason[0]) {
                        std::strncpy(firstScreenReason, candidate.reason, sizeof(firstScreenReason) - 1);
                    }
                }
                if (candidate.ok && (!best.ok || candidate.score > best.score)) {
                    best = candidate;
                }
            }

            selection = best;
            if (!selection.ok) {
                selection.screenRejected = sawScreenRejected;
                snprintf(
                    selection.reason,
                    sizeof(selection.reason),
                    "no world shader constant matrix in first %u registers%s%s",
                    SHADER_CONSTANT_REGISTER_COUNT,
                    firstScreenReason[0] ? "; first screen reject=" : "",
                    firstScreenReason[0] ? firstScreenReason : "");
            } else if (g_config.autoLockWvpRegister) {
                g_lockedWvpValid = true;
                g_lockedWvpRegister = selection.startRegister;
                g_lockedWvpTransposed = selection.transposed;
                LogAlways(
                    "wvp auto-lock register=%d transposed=%d score=%.3f",
                    g_lockedWvpRegister,
                    g_lockedWvpTransposed ? 1 : 0,
                    selection.score);
            }
            return selection.ok;
        }

        bool SelectShaderConstantWvpMatrix(IDirect3DDevice9* device, const VertexBounds& bounds, MatrixSelection& selection)
        {
            if (!device) {
                snprintf(selection.reason, sizeof(selection.reason), "device is null");
                return false;
            }

            std::array<float, SHADER_CONSTANT_REGISTER_COUNT * 4> constants{};
            const HRESULT hr = device->GetVertexShaderConstantF(0, constants.data(), SHADER_CONSTANT_REGISTER_COUNT);
            if (FAILED(hr)) {
                snprintf(selection.reason, sizeof(selection.reason), "GetVertexShaderConstantF failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            if (g_config.wvpRegister >= 0) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, constants.data() + g_config.wvpRegister * 4, sizeof(matrix));
                selection = EvaluateWvpMatrixCandidate(matrix, g_config.wvpRegister, "shaderConstantsWvp", viewportPtr, bounds);
                if (selection.ok) {
                    ++g_counters.shaderWvpSelected;
                    return true;
                }

                LogLiveShaderWvpReject(g_config.wvpRegister, selection);
                return false;
            }

            MatrixSelection best{};
            bool sawScreenRejected = false;
            char firstRejectReason[160]{};
            for (int reg = 0; reg <= static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT - 4); ++reg) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, constants.data() + reg * 4, sizeof(matrix));
                MatrixSelection candidate = EvaluateWvpMatrixCandidate(matrix, reg, "shaderConstantsWvp", viewportPtr, bounds);
                if (candidate.screenRejected) {
                    sawScreenRejected = true;
                }
                if (!candidate.ok && !firstRejectReason[0]) {
                    CopyText(firstRejectReason, sizeof(firstRejectReason), candidate.reason);
                    LogLiveShaderWvpReject(reg, candidate);
                }
                if (candidate.ok && (!best.ok || candidate.score > best.score)) {
                    best = candidate;
                }
            }

            selection = best;
            if (!selection.ok) {
                selection.screenRejected = sawScreenRejected;
                snprintf(
                    selection.reason,
                    sizeof(selection.reason),
                    "no bounds-valid shaderConstantsWvp in first %u registers%s%s",
                    SHADER_CONSTANT_REGISTER_COUNT,
                    firstRejectReason[0] ? "; first reject=" : "",
                    firstRejectReason[0] ? firstRejectReason : "");
                return false;
            }

            ++g_counters.shaderWvpSelected;
            return true;
        }

        bool ReadShaderConstants(IDirect3DDevice9* device, ShaderConstantsSnapshot& snapshot, char* reason, std::size_t reasonSize)
        {
            snapshot.valid = false;
            if (!device) {
                SetMatrixReason(reason, reasonSize, "device is null");
                return false;
            }

            const HRESULT hr = device->GetVertexShaderConstantF(0, snapshot.values.data(), SHADER_CONSTANT_REGISTER_COUNT);
            if (FAILED(hr)) {
                SetMatrixReason(reason, reasonSize, "GetVertexShaderConstantF failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            snapshot.valid = true;
            SetMatrixReason(reason, reasonSize, "shader constants captured");
            return true;
        }

        bool HasShaderRegisters(const RatatouilleShaderLayout& layout)
        {
            const int highestRegister = layout.omniRegister + layout.omniSize * layout.omniCount + 3;
            return layout.viewRegister >= 0 &&
                   layout.projectionRegister >= 0 &&
                   layout.worldRegister >= 0 &&
                   layout.materialRegister >= 0 &&
                   layout.lightRegister >= 0 &&
                   layout.omniRegister >= 0 &&
                   highestRegister < static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT);
        }

        D3DMATRIX MatrixFromShaderConstants(const ShaderConstantsSnapshot& snapshot, int startRegister, bool transposed)
        {
            D3DMATRIX matrix{};
            if (!snapshot.valid || startRegister < 0 ||
                startRegister > static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT - 4)) {
                return matrix;
            }

            std::memcpy(&matrix, snapshot.values.data() + startRegister * 4, sizeof(matrix));
            return transposed ? TransposeMatrix(matrix) : matrix;
        }

        const float* ShaderRegister(const ShaderConstantsSnapshot& snapshot, int startRegister)
        {
            if (!snapshot.valid || startRegister < 0 || startRegister >= static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT)) {
                return nullptr;
            }

            return snapshot.values.data() + startRegister * 4;
        }

        FixedFunctionTransformSet BuildLegacyTransformSet(const D3DMATRIX* transform, bool transformIsWorld)
        {
            FixedFunctionTransformSet transforms{};
            if (!transform) {
                return transforms;
            }

            transforms.valid = true;
            if (transformIsWorld) {
                transforms.setWorld = true;
                transforms.world = *transform;
                return transforms;
            }

            const D3DMATRIX identity = IdentityMatrix();
            transforms.setWorld = true;
            transforms.setView = true;
            transforms.setProjection = true;
            transforms.world = identity;
            transforms.view = identity;
            transforms.projection = *transform;
            return transforms;
        }

        FixedFunctionTransformSet AssembleRatatouilleTransformSet(
            const D3DMATRIX& world,
            const D3DMATRIX& view,
            const D3DMATRIX& projection)
        {
            FixedFunctionTransformSet transforms{};
            transforms.valid = true;
            transforms.setWorld = true;
            transforms.setView = true;
            transforms.setProjection = true;

            const D3DMATRIX identity = IdentityMatrix();
            switch (g_config.transformAssemblyMode) {
                case TransformAssemblyMode::FusedWorldView:
                    transforms.world = identity;
                    transforms.view = MultiplyMatrix(world, view);
                    transforms.projection = projection;
                    break;

                case TransformAssemblyMode::FullWvpProjection:
                    transforms.world = identity;
                    transforms.view = identity;
                    transforms.projection = MultiplyMatrix(MultiplyMatrix(world, view), projection);
                    break;

                default:
                    transforms.world = world;
                    transforms.view = view;
                    transforms.projection = projection;
                    break;
            }

            return transforms;
        }

        bool SelectRatatouilleShaderTransforms(
            IDirect3DDevice9* device,
            const ShaderConstantsSnapshot& snapshot,
            const VertexBounds& bounds,
            RatatouilleTransformSelection& selection)
        {
            if (!snapshot.valid) {
                SetMatrixReason(selection.reason, sizeof(selection.reason), "shader constants unavailable");
                return false;
            }

            if (!bounds.valid) {
                SetMatrixReason(selection.reason, sizeof(selection.reason), "bounds invalid");
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            char firstReject[160]{};
            for (const RatatouilleShaderLayout& layout : RATATOUILLE_SHADER_LAYOUTS) {
                if (!HasShaderRegisters(layout)) {
                    continue;
                }

                for (int transposeIndex = 0; transposeIndex < 2; ++transposeIndex) {
                    const bool transposed = transposeIndex != 0;
                    const D3DMATRIX world = MatrixFromShaderConstants(snapshot, layout.worldRegister, transposed);
                    const D3DMATRIX view = MatrixFromShaderConstants(snapshot, layout.viewRegister, transposed);
                    const D3DMATRIX projection = MatrixFromShaderConstants(snapshot, layout.projectionRegister, transposed);

                    ++g_counters.matrixCandidates;
                    ++g_counters.ratTransformCandidates;

                    char worldReason[96]{};
                    if (!ValidateAffineWorldMatrix(world, viewportPtr, worldReason, sizeof(worldReason))) {
                        if (!firstReject[0]) {
                            SetMatrixReason(
                                firstReject,
                                sizeof(firstReject),
                                "%s transposed=%d world rejected: %s",
                                layout.name,
                                transposed ? 1 : 0,
                                worldReason);
                        }
                        ++g_counters.ratTransformRejected;
                        continue;
                    }

                    char viewReason[96]{};
                    char projectionReason[96]{};
                    if (!MatrixValuesAreFinite(view, viewReason, sizeof(viewReason)) ||
                        !MatrixValuesAreFinite(projection, projectionReason, sizeof(projectionReason))) {
                        if (!firstReject[0]) {
                            SetMatrixReason(
                                firstReject,
                                sizeof(firstReject),
                                "%s transposed=%d view/projection rejected view=%s projection=%s",
                                layout.name,
                                transposed ? 1 : 0,
                                viewReason[0] ? viewReason : "ok",
                                projectionReason[0] ? projectionReason : "ok");
                        }
                        ++g_counters.ratTransformRejected;
                        continue;
                    }

                    const D3DMATRIX worldView = MultiplyMatrix(world, view);
                    const D3DMATRIX worldViewProjection = MultiplyMatrix(worldView, projection);
                    bool screenRejected = false;
                    char scoreReason[160]{};
                    const float score = ScoreWvpCandidateAgainstBounds(
                        worldViewProjection,
                        viewportPtr,
                        bounds,
                        screenRejected,
                        scoreReason,
                        sizeof(scoreReason));

                    if (screenRejected) {
                        selection.screenRejected = true;
                    }

                    if (score <= 0.0f) {
                        if (!firstReject[0]) {
                            SetMatrixReason(
                                firstReject,
                                sizeof(firstReject),
                                "%s transposed=%d projected bounds rejected: %s",
                                layout.name,
                                transposed ? 1 : 0,
                                scoreReason[0] ? scoreReason : "unknown");
                        }
                        ++g_counters.ratTransformRejected;
                        continue;
                    }

                    if (!selection.ok || score > selection.score) {
                        selection.ok = true;
                        selection.transposed = transposed;
                        selection.score = score;
                        selection.layout = &layout;
                        selection.transforms = AssembleRatatouilleTransformSet(world, view, projection);
                        SetMatrixReason(
                            selection.reason,
                            sizeof(selection.reason),
                            "%s assembly=%s transposed=%d score=%.3f %s",
                            layout.name,
                            TransformAssemblyModeName(g_config.transformAssemblyMode),
                            transposed ? 1 : 0,
                            score,
                            scoreReason[0] ? scoreReason : "");
                    }
                }
            }

            if (!selection.ok) {
                selection.affineRejected = firstReject[0] && std::strstr(firstReject, "world rejected") != nullptr;
                SetMatrixReason(
                    selection.reason,
                    sizeof(selection.reason),
                    "no Ratatouille shader constant layout matched%s%s",
                    firstReject[0] ? "; first reject=" : "",
                    firstReject[0] ? firstReject : "");
                return false;
            }

            ++g_counters.ratTransformSelected;
            if ((g_config.logFile || g_config.debugLog) && g_counters.ratTransformSelected <= 24) {
                LogAlways(
                    "ratatouille transform selected layout=%s transposed=%d score=%.3f reason=\"%s\"",
                    selection.layout ? selection.layout->name : "(none)",
                    selection.transposed ? 1 : 0,
                    selection.score,
                    selection.reason[0] ? selection.reason : "(none)");
            }
            return true;
        }

        bool SelectRatatouilleShaderCameraForWorld(
            IDirect3DDevice9* device,
            const ShaderConstantsSnapshot& snapshot,
            const D3DMATRIX& safeWorld,
            const VertexBounds& bounds,
            RatatouilleTransformSelection& selection)
        {
            if (!snapshot.valid) {
                SetMatrixReason(selection.reason, sizeof(selection.reason), "shader constants unavailable");
                return false;
            }

            if (!bounds.valid) {
                SetMatrixReason(selection.reason, sizeof(selection.reason), "bounds invalid");
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            char firstReject[160]{};
            for (const RatatouilleShaderLayout& layout : RATATOUILLE_SHADER_LAYOUTS) {
                if (!HasShaderRegisters(layout)) {
                    continue;
                }

                for (int transposeIndex = 0; transposeIndex < 2; ++transposeIndex) {
                    const bool transposed = transposeIndex != 0;
                    const D3DMATRIX view = MatrixFromShaderConstants(snapshot, layout.viewRegister, transposed);
                    const D3DMATRIX projection = MatrixFromShaderConstants(snapshot, layout.projectionRegister, transposed);

                    ++g_counters.matrixCandidates;
                    ++g_counters.ratTransformCandidates;

                    char viewReason[96]{};
                    char projectionReason[96]{};
                    if (!MatrixValuesAreFinite(view, viewReason, sizeof(viewReason)) ||
                        !MatrixValuesAreFinite(projection, projectionReason, sizeof(projectionReason))) {
                        if (!firstReject[0]) {
                            SetMatrixReason(
                                firstReject,
                                sizeof(firstReject),
                                "%s transposed=%d camera rejected view=%s projection=%s",
                                layout.name,
                                transposed ? 1 : 0,
                                viewReason[0] ? viewReason : "ok",
                                projectionReason[0] ? projectionReason : "ok");
                        }
                        ++g_counters.ratTransformRejected;
                        continue;
                    }

                    const D3DMATRIX worldViewProjection = MultiplyMatrix(MultiplyMatrix(safeWorld, view), projection);
                    bool screenRejected = false;
                    char scoreReason[160]{};
                    const float score = ScoreWvpCandidateAgainstBounds(
                        worldViewProjection,
                        viewportPtr,
                        bounds,
                        screenRejected,
                        scoreReason,
                        sizeof(scoreReason));

                    if (screenRejected) {
                        selection.screenRejected = true;
                    }

                    if (score <= 0.0f) {
                        if (!firstReject[0]) {
                            SetMatrixReason(
                                firstReject,
                                sizeof(firstReject),
                                "%s transposed=%d safe-world camera rejected: %s",
                                layout.name,
                                transposed ? 1 : 0,
                                scoreReason[0] ? scoreReason : "unknown");
                        }
                        ++g_counters.ratTransformRejected;
                        continue;
                    }

                    if (!selection.ok || score > selection.score) {
                        selection.ok = true;
                        selection.transposed = transposed;
                        selection.score = score;
                        selection.layout = &layout;
                        selection.transforms = AssembleRatatouilleTransformSet(safeWorld, view, projection);
                        SetMatrixReason(
                            selection.reason,
                            sizeof(selection.reason),
                            "%s safeWorldCamera assembly=%s transposed=%d score=%.3f %s",
                            layout.name,
                            TransformAssemblyModeName(g_config.transformAssemblyMode),
                            transposed ? 1 : 0,
                            score,
                            scoreReason[0] ? scoreReason : "");
                    }
                }
            }

            if (!selection.ok) {
                SetMatrixReason(
                    selection.reason,
                    sizeof(selection.reason),
                    "no Ratatouille shader camera matched safe world%s%s",
                    firstReject[0] ? "; first reject=" : "",
                    firstReject[0] ? firstReject : "");
                return false;
            }

            ++g_counters.ratTransformSelected;
            if ((g_config.logFile || g_config.debugLog) && g_counters.ratTransformSelected <= 24) {
                LogAlways(
                    "ratatouille shader camera selected layout=%s transposed=%d score=%.3f reason=\"%s\"",
                    selection.layout ? selection.layout->name : "(none)",
                    selection.transposed ? 1 : 0,
                    selection.score,
                    selection.reason[0] ? selection.reason : "(none)");
            }
            return true;
        }

        bool SelectMatrixProbeWvpMatrix(const VertexBounds& bounds, MatrixSelection& selection)
        {
            if (!bounds.valid) {
                snprintf(selection.reason, sizeof(selection.reason), "bounds invalid");
                return false;
            }

            MatrixSelection best{};
            char firstRejectReason[160]{};
            std::uint64_t bestSequence = 0;
            int considered = 0;

            for (const MatrixProbeCandidate& probe : g_matrixProbeCandidates) {
                if (!probe.valid) {
                    continue;
                }

                ++considered;
                MatrixSelection candidate = EvaluateWvpMatrixCandidate(
                    probe.matrix,
                    static_cast<int>(probe.caller & 0x7FFFFFFF),
                    probe.source,
                    nullptr,
                    bounds);
                if (!candidate.ok) {
                    ++g_counters.matrixProbeRejected;
                    if (!firstRejectReason[0]) {
                        CopyText(firstRejectReason, sizeof(firstRejectReason), candidate.reason);
                    }
                    continue;
                }

                char reason[160]{};
                snprintf(
                    reason,
                    sizeof(reason),
                    "%s caller=0x%p seq=%llu",
                    candidate.reason[0] ? candidate.reason : "matrix probe",
                    reinterpret_cast<void*>(probe.caller),
                    static_cast<unsigned long long>(probe.sequence));
                CopyText(candidate.reason, sizeof(candidate.reason), reason);

                if (!best.ok || candidate.score > best.score ||
                    (candidate.score == best.score && probe.sequence > bestSequence)) {
                    best = candidate;
                    bestSequence = probe.sequence;
                }
            }

            selection = best;
            if (!selection.ok) {
                snprintf(
                    selection.reason,
                    sizeof(selection.reason),
                    "no bounds-valid matrix probe candidate considered=%d%s%s",
                    considered,
                    firstRejectReason[0] ? "; first reject=" : "",
                    firstRejectReason[0] ? firstRejectReason : "");
                return false;
            }

            ++g_counters.matrixProbeSelected;
            return true;
        }

        void LogCommandConstantWvpProbe(IDirect3DDevice9* device, const VertexBounds& bounds, const char* trigger)
        {
            if ((!g_config.logFile && !g_config.debugLog) || !g_currentCommand || !bounds.valid) {
                return;
            }

            static std::uint32_t logged = 0;
            if (logged >= 24) {
                return;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            constexpr int commandConstantRegisters = static_cast<int>(0x70 / (sizeof(float) * 4));
            MatrixSelection best{};
            char firstReject[160]{};
            int rejected = 0;
            for (int reg = 0; reg <= commandConstantRegisters - 4; ++reg) {
                D3DMATRIX matrix{};
                std::memcpy(&matrix, g_currentCommand->constants + reg * sizeof(float) * 4, sizeof(matrix));
                MatrixSelection candidate = EvaluateWvpMatrixCandidate(matrix, reg, "commandConstantsProbe", viewportPtr, bounds);
                if (!candidate.ok) {
                    ++rejected;
                    if (!firstReject[0]) {
                        CopyText(firstReject, sizeof(firstReject), candidate.reason);
                    }
                    continue;
                }

                if (!best.ok || candidate.score > best.score) {
                    best = candidate;
                }
            }

            ++logged;
            if (best.ok) {
                LogAlways(
                    "command WVP probe trigger=%s selected register=%d transposed=%d score=%.3f reason=\"%s\" bounds=[%g,%g,%g]-[%g,%g,%g]",
                    trigger && trigger[0] ? trigger : "unknown",
                    best.startRegister,
                    best.transposed ? 1 : 0,
                    best.score,
                    best.reason[0] ? best.reason : "(none)",
                    bounds.minX,
                    bounds.minY,
                    bounds.minZ,
                    bounds.maxX,
                    bounds.maxY,
                    bounds.maxZ);
            } else {
                ++g_counters.commandWvpMissing;
                LogAlways(
                    "command WVP probe trigger=%s noCandidate registers=%d rejected=%d firstReject=\"%s\" bounds=[%g,%g,%g]-[%g,%g,%g]",
                    trigger && trigger[0] ? trigger : "unknown",
                    commandConstantRegisters,
                    rejected,
                    firstReject[0] ? firstReject : "none",
                    bounds.minX,
                    bounds.minY,
                    bounds.minZ,
                    bounds.maxX,
                    bounds.maxY,
                    bounds.maxZ);
            }
        }

        bool ValidateWorldProjectedBounds(
            IDirect3DDevice9* device,
            const D3DMATRIX& world,
            const VertexBounds& bounds,
            char* reason,
            std::size_t reasonSize)
        {
            if (!bounds.valid) {
                SetMatrixReason(reason, reasonSize, "bounds invalid");
                return false;
            }

            char viewProjectionReason[160]{};
            if (!ValidateDeviceViewProjection(device, viewProjectionReason, sizeof(viewProjectionReason))) {
                SetMatrixReason(reason, reasonSize, "%s", viewProjectionReason);
                return false;
            }

            D3DMATRIX view{};
            D3DMATRIX projection{};
            if (FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
                FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) {
                SetMatrixReason(reason, reasonSize, "GetTransform VIEW/PROJECTION failed during bounds validation");
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            const D3DMATRIX worldView = MultiplyMatrix(world, view);
            const D3DMATRIX worldViewProjection = MultiplyMatrix(worldView, projection);
            bool screenRejected = false;
            char scoreReason[160]{};
            const float score = ScoreWvpCandidateAgainstBounds(
                worldViewProjection,
                viewportPtr,
                bounds,
                screenRejected,
                scoreReason,
                sizeof(scoreReason));
            if (score <= 0.0f) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "world projected bounds rejected: %s",
                    scoreReason[0] ? scoreReason : "unknown");
                return false;
            }

            SetMatrixReason(
                reason,
                reasonSize,
                "world projected bounds ok score=%.3f %s",
                score,
                scoreReason[0] ? scoreReason : "");
            return true;
        }

        bool SelectRendererConstant5Matrix(const D3DRendererZ* renderer, IDirect3DDevice9* device, MatrixSelection& selection)
        {
            if (!renderer) {
                snprintf(selection.reason, sizeof(selection.reason), "renderer is null");
                return false;
            }

            const auto* rendererBytes = reinterpret_cast<const std::uint8_t*>(renderer);
            const auto* cacheBase = rendererBytes + RENDERER_CONSTANT_CACHE_OFFSET;
            const auto* slot = *reinterpret_cast<const std::uint8_t* const*>(cacheBase + RENDERER_CONSTANT5_SLOT_OFFSET);
            if (!slot) {
                snprintf(selection.reason, sizeof(selection.reason), "renderer constant slot 5 is null");
                return false;
            }

            D3DMATRIX matrix{};
            std::memcpy(&matrix, slot + RENDERER_CONSTANT_MATRIX_OFFSET, sizeof(matrix));
            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            selection = EvaluateMatrixCandidate(matrix, 5, "rendererConstant5", viewportPtr);
            return selection.ok;
        }

        void LogRendererSlot5Diagnostics(
            const D3DMATRIX& raw,
            const D3DMATRIX& transposed,
            const char* rawReason,
            const char* transposedReason,
            bool rawOk,
            bool transposedOk,
            bool selectedTransposed)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            static std::uint32_t logged = 0;
            if (logged >= 16) {
                return;
            }

            ++logged;
            LogAlways(
                "rendererSlot5World rawOk=%d transposedOk=%d selectedTransposed=%d rawReason=\"%s\" transposedReason=\"%s\" raw=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] transposed=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                rawOk ? 1 : 0,
                transposedOk ? 1 : 0,
                selectedTransposed ? 1 : 0,
                rawReason && rawReason[0] ? rawReason : "(none)",
                transposedReason && transposedReason[0] ? transposedReason : "(none)",
                raw._11, raw._12, raw._13, raw._14,
                raw._21, raw._22, raw._23, raw._24,
                raw._31, raw._32, raw._33, raw._34,
                raw._41, raw._42, raw._43, raw._44,
                transposed._11, transposed._12, transposed._13, transposed._14,
                transposed._21, transposed._22, transposed._23, transposed._24,
                transposed._31, transposed._32, transposed._33, transposed._34,
                transposed._41, transposed._42, transposed._43, transposed._44);
        }

        void LogDeviceTransformDiagnostics(IDirect3DDevice9* device)
        {
            if ((!g_config.logFile && !g_config.debugLog) || !device) {
                return;
            }

            static std::uint32_t logged = 0;
            if (logged >= 16) {
                return;
            }

            D3DMATRIX world{};
            D3DMATRIX view{};
            D3DMATRIX projection{};
            const HRESULT worldHr = device->GetTransform(D3DTS_WORLD, &world);
            const HRESULT viewHr = device->GetTransform(D3DTS_VIEW, &view);
            const HRESULT projectionHr = device->GetTransform(D3DTS_PROJECTION, &projection);

            ++logged;
            LogAlways(
                "device transforms worldHr=0x%08lx viewHr=0x%08lx projectionHr=0x%08lx world=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] view=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] projection=[%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                static_cast<unsigned long>(worldHr),
                static_cast<unsigned long>(viewHr),
                static_cast<unsigned long>(projectionHr),
                world._11, world._12, world._13, world._14,
                world._21, world._22, world._23, world._24,
                world._31, world._32, world._33, world._34,
                world._41, world._42, world._43, world._44,
                view._11, view._12, view._13, view._14,
                view._21, view._22, view._23, view._24,
                view._31, view._32, view._33, view._34,
                view._41, view._42, view._43, view._44,
                projection._11, projection._12, projection._13, projection._14,
                projection._21, projection._22, projection._23, projection._24,
                projection._31, projection._32, projection._33, projection._34,
                projection._41, projection._42, projection._43, projection._44);
        }

        bool SelectRendererSlot5WorldMatrix(const D3DRendererZ* renderer, IDirect3DDevice9* device, MatrixSelection& selection)
        {
            if (!renderer) {
                snprintf(selection.reason, sizeof(selection.reason), "renderer is null");
                return false;
            }

            const auto* rendererBytes = reinterpret_cast<const std::uint8_t*>(renderer);
            const auto* cacheBase = rendererBytes + RENDERER_CONSTANT_CACHE_OFFSET;
            const auto* slot = *reinterpret_cast<const std::uint8_t* const*>(cacheBase + RENDERER_CONSTANT5_SLOT_OFFSET);
            if (!slot) {
                snprintf(selection.reason, sizeof(selection.reason), "renderer slot 5 is null");
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport))) {
                viewportPtr = &viewport;
            }

            D3DMATRIX raw{};
            std::memcpy(&raw, slot + RENDERER_CONSTANT_MATRIX_OFFSET, sizeof(raw));
            const D3DMATRIX transposed = TransposeMatrix(raw);

            char rawReason[160]{};
            char transposedReason[160]{};
            const bool rawOk = ValidateAffineWorldMatrix(raw, viewportPtr, rawReason, sizeof(rawReason));
            const bool transposedOk = ValidateAffineWorldMatrix(transposed, viewportPtr, transposedReason, sizeof(transposedReason));

            const bool selectedTransposed = !rawOk && transposedOk;
            LogRendererSlot5Diagnostics(raw, transposed, rawReason, transposedReason, rawOk, transposedOk, selectedTransposed);
            LogDeviceTransformDiagnostics(device);

            selection.startRegister = 5;
            std::strncpy(selection.source, "rendererSlot5World", sizeof(selection.source) - 1);
            if (rawOk) {
                selection.ok = true;
                selection.matrix = raw;
                selection.transposed = false;
                selection.score = 1.0f;
                std::strncpy(selection.reason, rawReason, sizeof(selection.reason) - 1);
                return true;
            }

            if (transposedOk) {
                selection.ok = true;
                selection.matrix = transposed;
                selection.transposed = true;
                selection.score = 1.0f;
                std::strncpy(selection.reason, transposedReason, sizeof(selection.reason) - 1);
                return true;
            }

            selection.affineRejected = true;
            snprintf(
                selection.reason,
                sizeof(selection.reason),
                "renderer slot 5 rejected raw=%s transposed=%s",
                rawReason[0] ? rawReason : "unknown",
                transposedReason[0] ? transposedReason : "unknown");
            return false;
        }

        bool ReadRendererCachedMatrix(
            const D3DRendererZ* renderer,
            std::ptrdiff_t slotOffset,
            const char* slotName,
            D3DMATRIX& matrix,
            char* reason,
            std::size_t reasonSize)
        {
            if (!renderer) {
                SetMatrixReason(reason, reasonSize, "renderer is null");
                return false;
            }

            const auto* rendererBytes = reinterpret_cast<const std::uint8_t*>(renderer);
            const auto* cacheBase = rendererBytes + RENDERER_CONSTANT_CACHE_OFFSET;
            const auto* slotAddress = cacheBase + slotOffset;
            if (!IsReadableMemoryRange(slotAddress, sizeof(const std::uint8_t*))) {
                SetMatrixReason(reason, reasonSize, "%s pointer is unreadable", slotName);
                return false;
            }

            const auto* slot = *reinterpret_cast<const std::uint8_t* const*>(slotAddress);
            if (!slot || !IsReadableMemoryRange(slot + RENDERER_CONSTANT_MATRIX_OFFSET, sizeof(D3DMATRIX))) {
                SetMatrixReason(reason, reasonSize, "%s matrix is unavailable", slotName);
                return false;
            }

            D3DMATRIX shaderConvention{};
            std::memcpy(&shaderConvention, slot + RENDERER_CONSTANT_MATRIX_OFFSET, sizeof(shaderConvention));
            matrix = TransposeMatrix(shaderConvention);
            return true;
        }

        bool SelectRendererMatrixCacheMatrix(
            const D3DRendererZ* renderer,
            IDirect3DDevice9* device,
            MatrixSelection& selection,
            FixedFunctionTransformSet* transformSet = nullptr,
            bool* projectionOrthographic = nullptr)
        {
            D3DMATRIX view{};
            D3DMATRIX projection{};
            D3DMATRIX world{};
            char readReason[160]{};
            if (!ReadRendererCachedMatrix(renderer, RENDERER_CONSTANT3_SLOT_OFFSET, "renderer slot 3 VIEW", view, readReason, sizeof(readReason)) ||
                !ReadRendererCachedMatrix(renderer, RENDERER_CONSTANT4_SLOT_OFFSET, "renderer slot 4 PROJECTION", projection, readReason, sizeof(readReason)) ||
                !ReadRendererCachedMatrix(renderer, RENDERER_CONSTANT5_SLOT_OFFSET, "renderer slot 5 WORLD", world, readReason, sizeof(readReason))) {
                ++g_counters.rendererTripletInvalid;
                CopyText(selection.source, sizeof(selection.source), "rendererMatrixCache");
                CopyText(selection.reason, sizeof(selection.reason), readReason);
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const D3DVIEWPORT9* viewportPtr = nullptr;
            if (device && SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width != 0 && viewport.Height != 0) {
                viewportPtr = &viewport;
            }

            char worldReason[160]{};
            char viewReason[160]{};
            char projectionReason[160]{};
            bool orthographic = false;
            const bool worldOk = ValidateAffineWorldMatrix(world, viewportPtr, worldReason, sizeof(worldReason));
            const bool viewOk = ValidateRendererViewMatrix(view, viewReason, sizeof(viewReason));
            const bool projectionOk = ValidateRendererProjectionMatrix(
                projection, orthographic, projectionReason, sizeof(projectionReason));
            if (!worldOk || !viewOk || !projectionOk) {
                ++g_counters.rendererTripletInvalid;
                CopyText(selection.source, sizeof(selection.source), "rendererMatrixCache");
                SetMatrixReason(
                    selection.reason,
                    sizeof(selection.reason),
                    "renderer triplet rejected world=%s view=%s projection=%s",
                    worldReason[0] ? worldReason : "unknown",
                    viewReason[0] ? viewReason : "unknown",
                    projectionReason[0] ? projectionReason : "unknown");
                if (g_config.debugLog || g_counters.rendererTripletInvalid <= 16) {
                    LogAlways("%s", selection.reason);
                }
                return false;
            }

            ++g_counters.rendererTripletValid;
            selection.ok = true;
            selection.matrix = world;
            selection.transposed = true;
            selection.startRegister = 5;
            selection.score = 1.0f;
            CopyText(selection.source, sizeof(selection.source), "rendererMatrixCache");
            SetMatrixReason(
                selection.reason,
                sizeof(selection.reason),
                "renderer slots 3/4/5 %s projection",
                orthographic ? "orthographic" : "perspective");

            if (transformSet) {
                transformSet->valid = true;
                transformSet->setWorld = true;
                transformSet->setView = true;
                transformSet->setProjection = true;
                transformSet->world = world;
                transformSet->view = view;
                transformSet->projection = projection;
            }
            if (projectionOrthographic) {
                *projectionOrthographic = orthographic;
            }

            if (g_config.debugLog || g_counters.rendererTripletValid <= 16) {
                LogAlways(
                    "renderer triplet accepted projection=%s worldT=[%g,%g,%g] viewT=[%g,%g,%g] proj=[%g,%g,%g,%g]",
                    orthographic ? "orthographic" : "perspective",
                    world._41, world._42, world._43,
                    view._41, view._42, view._43,
                    projection._11, projection._22, projection._34, projection._44);
            }
            return true;
        }

        bool SelectTransformMatrix(IDirect3DDevice9* device, const D3DRendererZ* renderer, MatrixSelection& selection)
        {
            if (g_config.transformMode == RtxTransformMode::DeviceTransforms) {
                selection.ok = true;
                std::strncpy(selection.source, "deviceTransforms", sizeof(selection.source) - 1);
                return true;
            }

            if (g_config.transformMode == RtxTransformMode::ShaderConstantsWvp) {
                CopyText(selection.source, sizeof(selection.source), "shaderConstantsWvp");
                snprintf(selection.reason, sizeof(selection.reason), "shaderConstantsWvp requires repacked vertex bounds");
                return false;
            }

            bool ok = false;
            if (g_config.transformMode == RtxTransformMode::RendererSlot5World) {
                ok = SelectRendererSlot5WorldMatrix(renderer, device, selection);
            } else if (g_config.transformMode == RtxTransformMode::RendererMatrixCache) {
                ok = SelectRendererMatrixCacheMatrix(renderer, device, selection);
            } else if (g_config.transformMode == RtxTransformMode::RendererConstant5) {
                ok = SelectRendererConstant5Matrix(renderer, device, selection);
            } else {
                ok = SelectShaderConstantMatrix(device, selection);
            }

            if (ok) {
                LogMatrixSelection(selection);
                return true;
            }

            ++g_counters.matrixFailures;
            if (g_config.debugLog || g_counters.matrixFailures <= 8) {
                LogAlways("wvp fail mode=%s reason=\"%s\"", TransformModeName(g_config.transformMode), selection.reason);
            }
            return false;
        }

        ModuleInfo GetMainModule()
        {
            HMODULE mainModule = GetModuleHandleA(nullptr);
            MODULEINFO info{};
            GetModuleInformation(GetCurrentProcess(), mainModule, &info, sizeof(info));

            return {
                reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll),
                static_cast<std::size_t>(info.SizeOfImage)
            };
        }

        template<std::size_t N>
        std::uintptr_t FindUniqueSignature(std::uintptr_t base, std::size_t size, const Signature<N>& sig, const char* name)
        {
            std::uintptr_t first = 0;
            std::size_t hits = 0;
            const auto* memory = reinterpret_cast<const std::uint8_t*>(base);

            if (N > size) {
                LogAlways("%s signature is larger than module", name);
                return 0;
            }

            for (std::size_t i = 0; i <= size - N; ++i) {
                bool found = true;
                for (std::size_t j = 0; j < N; ++j) {
                    if (sig.mask[j] == 'x' && sig.bytes[j] != memory[i + j]) {
                        found = false;
                        break;
                    }
                }

                if (found) {
                    if (!first) {
                        first = base + i;
                    }
                    ++hits;
                }
            }

            if (hits != 1) {
                LogAlways("%s signature resolved %llu matches", name, static_cast<unsigned long long>(hits));
                return 0;
            }

            LogAlways("%s signature resolved at 0x%p", name, reinterpret_cast<void*>(first));
            return first;
        }

        bool ResolveCallTarget(std::uintptr_t callAddress, std::uintptr_t& target)
        {
            const auto* instruction = reinterpret_cast<const std::uint8_t*>(callAddress);
            if (instruction[0] != 0xE8) {
                return false;
            }

            std::int32_t relative = 0;
            std::memcpy(&relative, instruction + 1, sizeof(relative));
            target = callAddress + 5 + relative;
            return true;
        }

        bool ResolveBridgeAddresses()
        {
            const ModuleInfo module = GetMainModule();
            g_addresses.queueDrain = FindUniqueSignature(module.base, module.size, rtxRenderQueueDrainSig, "RenderQueueDrain");
            g_addresses.renderCommand = FindUniqueSignature(module.base, module.size, rtxRenderCommandSig, "RenderCommand");
            g_addresses.drawPrimitiveBuffer = FindUniqueSignature(module.base, module.size, rtxDrawPrimitiveBufferSig, "DrawPrimitiveBuffer");
            g_addresses.drawPrimitiveUp = FindUniqueSignature(module.base, module.size, rtxDrawPrimitiveUpSig, "DrawPrimitiveUP");
            g_addresses.setStream = FindUniqueSignature(module.base, module.size, rtxSetStreamSig, "SetStream");
            g_addresses.setIndices = FindUniqueSignature(module.base, module.size, rtxSetIndicesSig, "SetIndices");
            g_addresses.matrixMultiplyHelper = FindUniqueSignature(module.base, module.size, rtxMatrixMultiplyHelperSig, "MatrixMultiplyHelper");
            g_addresses.lookAtHelper = FindUniqueSignature(module.base, module.size, rtxLookAtHelperSig, "LookAtHelper");

            if (!g_addresses.queueDrain || !g_addresses.renderCommand || !g_addresses.drawPrimitiveBuffer ||
                !g_addresses.drawPrimitiveUp || !g_addresses.setStream || !g_addresses.setIndices) {
                return false;
            }

            const std::uintptr_t draw = g_addresses.drawPrimitiveBuffer;
            return ResolveCallTarget(draw + 0x2A, g_addresses.applyShaderConstant) &&
                   ResolveCallTarget(draw + 0x68, g_addresses.computeSplitCount) &&
                   ResolveCallTarget(draw + 0xC7, g_addresses.advanceDrawPass);
        }

        bool IsSupportedPrimitiveType(std::uint16_t primitiveType)
        {
            switch (static_cast<D3DPRIMITIVETYPE>(primitiveType)) {
            case D3DPT_POINTLIST:
            case D3DPT_LINELIST:
            case D3DPT_LINESTRIP:
            case D3DPT_TRIANGLELIST:
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN:
                return true;
            default:
                return false;
            }
        }

        bool IsTrianglePrimitiveType(std::uint16_t primitiveType)
        {
            switch (static_cast<D3DPRIMITIVETYPE>(primitiveType)) {
            case D3DPT_TRIANGLELIST:
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN:
                return true;
            default:
                return false;
            }
        }

        int DeclTypeByteSize(BYTE type)
        {
            switch (type) {
            case D3DDECLTYPE_FLOAT1: return 4;
            case D3DDECLTYPE_FLOAT2: return 8;
            case D3DDECLTYPE_FLOAT3: return 12;
            case D3DDECLTYPE_FLOAT4: return 16;
            case D3DDECLTYPE_D3DCOLOR: return 4;
            case D3DDECLTYPE_UBYTE4: return 4;
            case D3DDECLTYPE_SHORT2: return 4;
            case D3DDECLTYPE_SHORT4: return 8;
            case D3DDECLTYPE_UBYTE4N: return 4;
            case D3DDECLTYPE_SHORT2N: return 4;
            case D3DDECLTYPE_SHORT4N: return 8;
            case D3DDECLTYPE_USHORT2N: return 4;
            case D3DDECLTYPE_USHORT4N: return 8;
            case D3DDECLTYPE_UDEC3: return 4;
            case D3DDECLTYPE_DEC3N: return 4;
            case D3DDECLTYPE_FLOAT16_2: return 4;
            case D3DDECLTYPE_FLOAT16_4: return 8;
            default: return 0;
            }
        }

        int DeclTypeComponentCount(BYTE type)
        {
            switch (type) {
            case D3DDECLTYPE_FLOAT1: return 1;
            case D3DDECLTYPE_FLOAT2: return 2;
            case D3DDECLTYPE_FLOAT3: return 3;
            case D3DDECLTYPE_FLOAT4: return 4;
            default: return 0;
            }
        }

        bool AddTexCoordSize(DWORD& fvf, int usageIndex, int componentCount)
        {
            switch (componentCount) {
            case 1: fvf |= D3DFVF_TEXCOORDSIZE1(usageIndex); return true;
            case 2: return true;
            case 3: fvf |= D3DFVF_TEXCOORDSIZE3(usageIndex); return true;
            case 4: fvf |= D3DFVF_TEXCOORDSIZE4(usageIndex); return true;
            default: return false;
            }
        }

        bool SetPositionFvf(DWORD& fvf, bool rhw, int blendSlots, bool lastBetaUByte4)
        {
            if (rhw) {
                if (blendSlots != 0 || lastBetaUByte4) {
                    return false;
                }
                fvf |= D3DFVF_XYZRHW;
                return true;
            }

            switch (blendSlots) {
            case 0: fvf |= D3DFVF_XYZ; break;
            case 1: fvf |= D3DFVF_XYZB1; break;
            case 2: fvf |= D3DFVF_XYZB2; break;
            case 3: fvf |= D3DFVF_XYZB3; break;
            case 4: fvf |= D3DFVF_XYZB4; break;
            case 5: fvf |= D3DFVF_XYZB5; break;
            default: return false;
            }

            if (lastBetaUByte4) {
                fvf |= D3DFVF_LASTBETA_UBYTE4;
            }

            return true;
        }

        void SetFvfReason(FvfBuildResult& result, const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(result.reason, sizeof(result.reason), fmt, args);
            va_end(args);
        }

        void AppendText(char* buffer, std::size_t bufferSize, std::size_t& used, const char* fmt, ...)
        {
            if (used >= bufferSize) {
                return;
            }

            va_list args;
            va_start(args, fmt);
            const int written = vsnprintf(buffer + used, bufferSize - used, fmt, args);
            va_end(args);

            if (written <= 0) {
                return;
            }

            used += std::min<std::size_t>(static_cast<std::size_t>(written), bufferSize - used - 1);
        }

        void HashByte(std::uint32_t& hash, std::uint8_t value)
        {
            hash ^= value;
            hash *= 16777619u;
        }

        template<typename T>
        void HashValue(std::uint32_t& hash, T value)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                HashByte(hash, bytes[i]);
            }
        }

        std::uint32_t HashProxyEvent(const PrimitiveDraw* draw, int passMode, const FixedFunctionPassResult& result)
        {
            std::uint32_t hash = 2166136261u;
            HashValue(hash, passMode);
            HashValue(hash, draw ? draw->primitiveType : 0);
            HashValue(hash, draw ? draw->stride : 0);
            HashValue(hash, draw && draw->indexBuffer ? 1 : 0);
            HashValue(hash, result.status);
            HashValue(hash, result.reason);
            HashValue(hash, result.path);
            HashValue(hash, result.hasTexture);
            HashValue(hash, result.proxyOnly);
            HashValue(hash, result.visibleProxy);
            HashValue(hash, result.colorWritesEnabled);
            HashValue(hash, result.noOpBlend);
            HashValue(hash, result.alphaTestEnabled);
            HashValue(hash, result.colorWriteMask);
            HashValue(hash, result.zEnable);
            HashValue(hash, result.zWriteEnable);
            HashValue(hash, result.alphaBlendEnable);
            HashValue(hash, result.textureWidth);
            HashValue(hash, result.textureHeight);
            HashValue(hash, result.textureScreenSized);
            HashValue(hash, result.textureRenderTarget);
            HashValue(hash, result.directFvfOk);
            HashValue(hash, result.fvf);
            HashValue(hash, result.mappedStride);
            HashValue(hash, static_cast<int>(result.proxyClass));
            return hash;
        }

        void LogProxyEvent(const char* outcome, const PrimitiveDraw* draw, int passMode, const FixedFunctionPassResult& result)
        {
            if ((!g_config.logFile && !g_config.debugLog) || !draw) {
                return;
            }

            const std::uint32_t hash = HashProxyEvent(draw, passMode, result);
            for (std::size_t i = 0; i < g_loggedProxyEventCount; ++i) {
                if (g_loggedProxyEventHashes[i] == hash) {
                    return;
                }
            }

            if (g_loggedProxyEventCount >= g_loggedProxyEventHashes.size()) {
                if (g_suppressedProxyEventLogs++ == 0) {
                    LogAlways("proxy draw log limit reached; suppressing additional unique proxy events");
                }
                return;
            }

            g_loggedProxyEventHashes[g_loggedProxyEventCount++] = hash;
            LogAlways(
                "proxy %s status=%s reason=%s path=%s class=%s passMode=%d primitive=%u stride=%u indexed=%d startVertex=%u minVertex=%u numVertices=%u startIndex=%u primitiveCount=%u proxyOnly=%d visibleProxy=%d colorWrite=%d noOpBlend=%d alphaTest=%d hasTexture=%d texSize=%ux%u texUsage=0x%08lx texScreen=%d texRt=%d stateValid=%d colorMask=0x%08lx zEnable=%lu zWrite=%lu alphaBlend=%lu srcBlend=%lu dstBlend=%lu directFvfOk=%d fvf=0x%08lx mappedStride=%d transform=%s layout=%s transposed=%d world=%d hr=0x%08lx detail=\"%s\" fvfReason=\"%s\" repackReason=\"%s\" transformReason=\"%s\"",
                outcome ? outcome : "event",
                FixedFunctionPassStatusName(result.status),
                FixedFunctionPassReasonName(result.reason),
                FixedFunctionPassPathName(result.path),
                ProxyDrawClassName(result.proxyClass),
                passMode,
                draw->primitiveType,
                draw->stride,
                draw->indexBuffer ? 1 : 0,
                draw->startVertex,
                draw->minVertexIndex,
                draw->numVertices,
                draw->startIndex,
                draw->primitiveCount,
                result.proxyOnly ? 1 : 0,
                result.visibleProxy ? 1 : 0,
                result.colorWritesEnabled ? 1 : 0,
                result.noOpBlend ? 1 : 0,
                result.alphaTestEnabled ? 1 : 0,
                result.hasTexture ? 1 : 0,
                result.textureWidth,
                result.textureHeight,
                static_cast<unsigned long>(result.textureUsage),
                result.textureScreenSized ? 1 : 0,
                result.textureRenderTarget ? 1 : 0,
                result.renderStateSnapshotValid ? 1 : 0,
                static_cast<unsigned long>(result.colorWriteMask),
                static_cast<unsigned long>(result.zEnable),
                static_cast<unsigned long>(result.zWriteEnable),
                static_cast<unsigned long>(result.alphaBlendEnable),
                static_cast<unsigned long>(result.srcBlend),
                static_cast<unsigned long>(result.dstBlend),
                result.directFvfOk ? 1 : 0,
                static_cast<unsigned long>(result.fvf),
                result.mappedStride,
                result.transformSource[0] ? result.transformSource : "(none)",
                result.shaderLayout[0] ? result.shaderLayout : "(none)",
                result.transformTransposed ? 1 : 0,
                result.transformIsWorld ? 1 : 0,
                static_cast<unsigned long>(result.hr),
                result.detail[0] ? result.detail : "(none)",
                result.fvfReason[0] ? result.fvfReason : "(none)",
                result.repackReason[0] ? result.repackReason : "(none)",
                result.transformReason[0] ? result.transformReason : "(none)");
        }

        std::uint32_t HashProxySubmissionIdentity(const PrimitiveDraw* draw, int passMode)
        {
            std::uint32_t hash = 2166136261u;
            HashValue(hash, reinterpret_cast<std::uintptr_t>(g_currentCommand));
            HashValue(hash, reinterpret_cast<std::uintptr_t>(draw));
            HashValue(hash, passMode);
            if (draw) {
                HashValue(hash, reinterpret_cast<std::uintptr_t>(draw->vertexBuffer));
                HashValue(hash, reinterpret_cast<std::uintptr_t>(draw->indexBuffer));
                HashValue(hash, reinterpret_cast<std::uintptr_t>(draw->shaderConstant));
                HashValue(hash, draw->primitiveType);
                HashValue(hash, draw->stride);
                HashValue(hash, draw->minVertexIndex);
                HashValue(hash, draw->numVertices);
                HashValue(hash, draw->startIndex);
                HashValue(hash, draw->primitiveCount);
                HashValue(hash, draw->indexBindParam);
                HashValue(hash, draw->startVertex);
            }

            return hash;
        }

        bool ShouldSkipDuplicateProxySubmission(const PrimitiveDraw* draw, int passMode, FixedFunctionPassResult& result)
        {
            if (!g_config.proxyDeduplicateCommandDraws || !draw || !g_currentCommand) {
                return false;
            }

            if (g_proxyDedupeCommand != g_currentCommand) {
                g_proxyDedupeCommand = g_currentCommand;
                g_proxyDedupeHashCount = 0;
            }

            const std::uint32_t hash = HashProxySubmissionIdentity(draw, passMode);
            for (std::size_t i = 0; i < g_proxyDedupeHashCount; ++i) {
                if (g_proxyDedupeHashes[i] == hash) {
                    ++g_counters.proxySkippedDuplicateCommand;
                    result.proxyOnly = true;
                    result.status = FixedFunctionPassStatus::SkippedUi;
                    result.hr = S_OK;
                    result.reason = FixedFunctionPassReason::SkippedDuplicateCommand;
                    SetPassDetail(result, "duplicate proxy submission for active render command");
                    return true;
                }
            }

            if (g_proxyDedupeHashCount < g_proxyDedupeHashes.size()) {
                g_proxyDedupeHashes[g_proxyDedupeHashCount++] = hash;
            }

            return false;
        }

        std::uint32_t HashDeclaration(const FvfBuildResult& result, std::uint16_t expectedStride)
        {
            std::uint32_t hash = 2166136261u;
            HashValue(hash, expectedStride);
            HashValue(hash, result.elementCount);
            HashValue(hash, result.ok);
            HashValue(hash, result.rhw);
            HashValue(hash, result.fvf);

            for (UINT i = 0; i < result.elementCount; ++i) {
                const D3DVERTEXELEMENT9& element = result.elements[i];
                HashValue(hash, element.Stream);
                HashValue(hash, element.Offset);
                HashValue(hash, element.Type);
                HashValue(hash, element.Method);
                HashValue(hash, element.Usage);
                HashValue(hash, element.UsageIndex);
            }

            return hash;
        }

        void LogFvfAttempt(const FvfBuildResult& result, std::uint16_t expectedStride)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            const std::uint32_t hash = HashDeclaration(result, expectedStride);
            for (std::size_t i = 0; i < g_loggedDeclarationCount; ++i) {
                if (g_loggedDeclarationHashes[i] == hash) {
                    return;
                }
            }

            if (g_loggedDeclarationCount >= g_loggedDeclarationHashes.size()) {
                if (g_suppressedDeclarationLogs++ == 0) {
                    LogAlways("fvf declaration log limit reached; suppressing additional unique declarations");
                }
                return;
            }

            g_loggedDeclarationHashes[g_loggedDeclarationCount++] = hash;

            char elementText[768]{};
            std::size_t used = 0;
            for (UINT i = 0; i < result.elementCount; ++i) {
                const D3DVERTEXELEMENT9& element = result.elements[i];
                AppendText(
                    elementText,
                    sizeof(elementText),
                    used,
                    "%s{s=%u off=%u type=%u method=%u usage=%u idx=%u}",
                    i == 0 ? "" : " ",
                    element.Stream,
                    element.Offset,
                    element.Type,
                    element.Method,
                    element.Usage,
                    element.UsageIndex);
            }

            LogAlways(
                "fvf %s stride=%u mapped=%d fvf=0x%08lx rhw=%d reason=\"%s\" elements=%u %s",
                result.ok ? "ok" : "fail",
                expectedStride,
                result.mappedStride,
                static_cast<unsigned long>(result.fvf),
                result.rhw ? 1 : 0,
                result.reason[0] ? result.reason : "(none)",
                result.elementCount,
                elementText[0] ? elementText : "(none)");
        }

        bool BuildFvfFromDeclaration(IDirect3DDevice9* device, std::uint16_t expectedStride, DWORD& fvfOut, FvfBuildResult& result)
        {
            IDirect3DVertexDeclaration9* declaration = nullptr;
            if (FAILED(device->GetVertexDeclaration(&declaration)) || !declaration) {
                DWORD currentFvf = 0;
                if (SUCCEEDED(device->GetFVF(&currentFvf)) && currentFvf != 0) {
                    result.ok = true;
                    result.rhw = IsRhwFvf(currentFvf);
                    result.fvf = currentFvf;
                    result.mappedStride = expectedStride;
                    SetFvfReason(result, "using existing FVF state");
                    fvfOut = currentFvf;
                    return true;
                }
                SetFvfReason(result, "no vertex declaration or FVF state");
                return false;
            }

            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH]{};
            UINT count = MAXD3DDECLLENGTH;
            const HRESULT hr = declaration->GetDeclaration(elements, &count);
            declaration->Release();
            if (FAILED(hr)) {
                SetFvfReason(result, "GetDeclaration failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH> sorted{};
            std::size_t sortedCount = 0;
            for (UINT i = 0; i < count; ++i) {
                if (elements[i].Stream == 0xFF || elements[i].Type == D3DDECLTYPE_UNUSED) {
                    break;
                }

                if (elements[i].Stream != 0 || sortedCount >= sorted.size()) {
                    SetFvfReason(result, "unsupported stream=%u at declaration index=%u", elements[i].Stream, i);
                    return false;
                }

                sorted[sortedCount++] = elements[i];
            }

            result.elementCount = static_cast<UINT>(std::min<std::size_t>(sortedCount, MAXD3DDECLLENGTH));
            for (UINT i = 0; i < result.elementCount; ++i) {
                result.elements[i] = sorted[i];
            }

            std::sort(sorted.begin(), sorted.begin() + sortedCount, [](const auto& lhs, const auto& rhs) {
                return lhs.Offset < rhs.Offset;
            });

            DWORD fvf = 0;
            std::size_t index = 0;
            int cursor = 0;
            int texCoordCount = 0;
            int blendSlots = 0;
            int blendWeightComponents = 0;
            bool positionRhw = false;
            bool lastBetaUByte4 = false;

            auto peekAtCursor = [&]() -> const D3DVERTEXELEMENT9* {
                if (index >= sortedCount) {
                    return nullptr;
                }

                const D3DVERTEXELEMENT9& element = sorted[index];
                if (element.Offset < cursor) {
                    SetFvfReason(result, "overlapping element at offset=%u cursor=%d", element.Offset, cursor);
                    return nullptr;
                }

                if (element.Offset > cursor) {
                    return nullptr;
                }

                return &element;
            };

            auto consumeCurrent = [&](int byteSize) {
                cursor += byteSize;
                ++index;
            };

            if (sortedCount == 0) {
                SetFvfReason(result, "empty vertex declaration");
                return false;
            }

            const D3DVERTEXELEMENT9& position = sorted[index];
            if (position.Offset != 0 || position.UsageIndex != 0) {
                SetFvfReason(result, "missing position at offset 0");
                return false;
            }

            if (position.Usage == D3DDECLUSAGE_POSITION && position.Type == D3DDECLTYPE_FLOAT3) {
                consumeCurrent(12);
            } else if (position.Usage == D3DDECLUSAGE_POSITIONT && position.Type == D3DDECLTYPE_FLOAT4) {
                positionRhw = true;
                result.rhw = true;
                consumeCurrent(16);
            } else {
                SetFvfReason(result, "unsupported position usage=%u type=%u", position.Usage, position.Type);
                return false;
            }

            if (const D3DVERTEXELEMENT9* element = peekAtCursor();
                element && element->Usage == D3DDECLUSAGE_BLENDWEIGHT && element->UsageIndex == 0 && !positionRhw) {
                const int componentCount = DeclTypeComponentCount(element->Type);
                if (componentCount < 1 || componentCount > 4) {
                    SetFvfReason(result, "unsupported blend weight type=%u", element->Type);
                    return false;
                }
                blendWeightComponents = componentCount;
                blendSlots = componentCount;
                cursor += blendWeightComponents * static_cast<int>(sizeof(float));
                ++index;
            }

            if (const D3DVERTEXELEMENT9* element = peekAtCursor();
                element && element->Usage == D3DDECLUSAGE_BLENDINDICES && element->UsageIndex == 0 && blendWeightComponents > 0) {
                if (element->Type != D3DDECLTYPE_UBYTE4 && element->Type != D3DDECLTYPE_D3DCOLOR) {
                    SetFvfReason(result, "unsupported blend index type=%u", element->Type);
                    return false;
                }
                blendSlots = blendWeightComponents + 1;
                lastBetaUByte4 = true;
                consumeCurrent(4);
            }

            if (!SetPositionFvf(fvf, positionRhw, blendSlots, lastBetaUByte4)) {
                SetFvfReason(result, "unsupported position FVF blendSlots=%d rhw=%d", blendSlots, positionRhw ? 1 : 0);
                return false;
            }

            if (const D3DVERTEXELEMENT9* element = peekAtCursor();
                element && element->Usage == D3DDECLUSAGE_NORMAL && element->UsageIndex == 0) {
                if (positionRhw || element->Type != D3DDECLTYPE_FLOAT3) {
                    SetFvfReason(result, "unsupported normal type=%u rhw=%d", element->Type, positionRhw ? 1 : 0);
                    return false;
                }
                fvf |= D3DFVF_NORMAL;
                consumeCurrent(12);
            }

            if (const D3DVERTEXELEMENT9* element = peekAtCursor();
                element && element->Usage == D3DDECLUSAGE_COLOR && element->UsageIndex == 0) {
                if (element->Type != D3DDECLTYPE_D3DCOLOR) {
                    SetFvfReason(result, "unsupported diffuse type=%u", element->Type);
                    return false;
                }
                fvf |= D3DFVF_DIFFUSE;
                consumeCurrent(4);
            }

            if (const D3DVERTEXELEMENT9* element = peekAtCursor();
                element && element->Usage == D3DDECLUSAGE_COLOR && element->UsageIndex == 1) {
                if (element->Type != D3DDECLTYPE_D3DCOLOR) {
                    SetFvfReason(result, "unsupported specular type=%u", element->Type);
                    return false;
                }
                fvf |= D3DFVF_SPECULAR;
                consumeCurrent(4);
            }

            while (texCoordCount < 8) {
                const D3DVERTEXELEMENT9* element = peekAtCursor();
                if (!element || element->Usage != D3DDECLUSAGE_TEXCOORD || element->UsageIndex != texCoordCount) {
                    break;
                }

                const int componentCount = DeclTypeComponentCount(element->Type);
                if (!AddTexCoordSize(fvf, texCoordCount, componentCount)) {
                    SetFvfReason(result, "unsupported texcoord%u type=%u", texCoordCount, element->Type);
                    return false;
                }

                ++texCoordCount;
                cursor += componentCount * static_cast<int>(sizeof(float));
                ++index;
            }

            fvf |= static_cast<DWORD>(texCoordCount) << D3DFVF_TEXCOUNT_SHIFT;

            if (expectedStride != 0 && cursor > expectedStride) {
                SetFvfReason(result, "mapped stride %d exceeds original stride %u", cursor, expectedStride);
                return false;
            }

            if (index < sortedCount) {
                const D3DVERTEXELEMENT9& trailing = sorted[index];
                if (trailing.Offset < cursor) {
                    SetFvfReason(result, "trailing overlap offset=%u cursor=%d", trailing.Offset, cursor);
                    return false;
                }

                if (trailing.Offset == cursor) {
                    SetFvfReason(
                        result,
                        "unsupported direct-FVF prefix element offset=%u usage=%u idx=%u type=%u",
                        trailing.Offset,
                        trailing.Usage,
                        trailing.UsageIndex,
                        trailing.Type);
                    return false;
                }

                SetFvfReason(
                    result,
                    "mapped prefix; ignored trailing offset=%u usage=%u type=%u",
                    trailing.Offset,
                    trailing.Usage,
                    trailing.Type);
            } else if (expectedStride != 0 && cursor < expectedStride) {
                SetFvfReason(result, "mapped prefix stride=%d original stride=%u", cursor, expectedStride);
            } else {
                SetFvfReason(result, "mapped full declaration");
            }

            result.ok = true;
            result.rhw = positionRhw || IsRhwFvf(fvf);
            result.fvf = fvf;
            result.mappedStride = cursor;
            fvfOut = fvf;
            return true;
        }

        void SetRepackReason(RepackLayout& layout, const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(layout.reason, sizeof(layout.reason), fmt, args);
            va_end(args);
        }

        bool BuildRepackLayout(IDirect3DDevice9* device, std::uint16_t expectedStride, RepackLayout& layout)
        {
            IDirect3DVertexDeclaration9* declaration = nullptr;
            if (FAILED(device->GetVertexDeclaration(&declaration)) || !declaration) {
                SetRepackReason(layout, "no vertex declaration");
                return false;
            }

            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH]{};
            UINT count = MAXD3DDECLLENGTH;
            const HRESULT hr = declaration->GetDeclaration(elements, &count);
            declaration->Release();
            if (FAILED(hr)) {
                SetRepackReason(layout, "GetDeclaration failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            for (UINT i = 0; i < count; ++i) {
                const D3DVERTEXELEMENT9& element = elements[i];
                if (element.Stream == 0xFF || element.Type == D3DDECLTYPE_UNUSED) {
                    break;
                }

                if (element.Stream != 0) {
                    SetRepackReason(layout, "unsupported stream=%u at declaration index=%u", element.Stream, i);
                    return false;
                }

                if (layout.elementCount < MAXD3DDECLLENGTH) {
                    layout.elements[layout.elementCount++] = element;
                }

                if (element.Usage == D3DDECLUSAGE_BLENDINDICES) {
                    layout.hasBlendIndices = true;
                    layout.blendIndexOffset = element.Offset;
                    layout.blendIndexType = element.Type;
                    layout.blendIndexComponents = DeclTypeComponentCount(element.Type);
                } else if (element.Usage == D3DDECLUSAGE_BLENDWEIGHT) {
                    layout.hasBlendWeight = true;
                    layout.blendWeightOffset = element.Offset;
                    layout.blendWeightType = element.Type;
                    layout.blendWeightComponents = DeclTypeComponentCount(element.Type);
                } else if (element.Usage == D3DDECLUSAGE_POSITION) {
                    if (element.UsageIndex == 1) {
                        layout.hasPosition1 = true;
                    } else if (element.UsageIndex == 2) {
                        layout.hasPosition2 = true;
                        if (layout.blendWeightOffset < 0) {
                            layout.blendWeightOffset = element.Offset;
                            layout.blendWeightType = element.Type;
                            layout.blendWeightComponents = DeclTypeComponentCount(element.Type);
                        }
                    } else if (element.UsageIndex >= 3) {
                        layout.hasPosition3 = true;
                    }
                } else if (element.Usage == D3DDECLUSAGE_NORMAL && element.UsageIndex == 0) {
                    layout.hasNormalSemantic = true;
                } else if (element.Usage == D3DDECLUSAGE_COLOR) {
                    layout.hasColorSemantic = true;
                }

                const int byteSize = DeclTypeByteSize(element.Type);
                if (byteSize <= 0 || expectedStride == 0 || element.Offset + byteSize > expectedStride) {
                    SetRepackReason(layout, "invalid element offset=%u type=%u stride=%u", element.Offset, element.Type, expectedStride);
                    return false;
                }

                if (element.Usage == D3DDECLUSAGE_POSITION && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_FLOAT3) {
                    layout.positionOffset = element.Offset;
                } else if (element.Usage == D3DDECLUSAGE_POSITIONT && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_FLOAT4) {
                    layout.rhw = true;
                    layout.positionOffset = element.Offset;
                } else if (element.Usage == D3DDECLUSAGE_NORMAL && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_FLOAT3) {
                    layout.hasNormal = true;
                    layout.normalOffset = element.Offset;
                    layout.normalType = element.Type;
                } else if (element.Usage == D3DDECLUSAGE_NORMAL && element.UsageIndex == 0 && element.Type == D3DDECLTYPE_D3DCOLOR) {
                    layout.hasNormal = true;
                    layout.normalOffset = element.Offset;
                    layout.normalType = element.Type;
                } else if (element.Usage == D3DDECLUSAGE_COLOR && !layout.hasDiffuse && element.Type == D3DDECLTYPE_D3DCOLOR) {
                    layout.hasDiffuse = true;
                    layout.diffuseOffset = element.Offset;
                } else if (element.Usage == D3DDECLUSAGE_TEXCOORD && element.UsageIndex == 0 && !layout.hasTexcoord) {
                    const int componentCount = DeclTypeComponentCount(element.Type);
                    if (componentCount >= 2 && componentCount <= 4) {
                        layout.hasTexcoord = true;
                        layout.texcoordOffset = element.Offset;
                        layout.texcoordComponents = componentCount;
                    }
                }
            }

            layout.hasPatchLikePositionSet = layout.hasPosition2 || layout.hasPosition3;

            if (layout.positionOffset < 0) {
                SetRepackReason(layout, "missing POSITION0 FLOAT3");
                return false;
            }

            layout.ok = true;
            SetRepackReason(
                layout,
                "position=%d normal=%d normalType=%d normalSemantic=%d diffuse=%d texcoord=%d texComponents=%d blend=%d indices=%d/%d weights=%d/%d patchLike=%d",
                layout.positionOffset,
                layout.normalOffset,
                layout.normalType,
                layout.hasNormalSemantic ? 1 : 0,
                layout.diffuseOffset,
                layout.texcoordOffset,
                layout.texcoordComponents,
                (layout.hasBlendIndices || layout.hasBlendWeight) ? 1 : 0,
                layout.blendIndexOffset,
                layout.blendIndexComponents,
                layout.blendWeightOffset,
                layout.blendWeightComponents,
                layout.hasPatchLikePositionSet ? 1 : 0);
            return true;
        }

        std::uint32_t HashRepackLayout(const RepackLayout& layout, std::uint16_t expectedStride, bool hasTexture)
        {
            std::uint32_t hash = 2166136261u;
            HashValue(hash, expectedStride);
            HashValue(hash, hasTexture);
            HashValue(hash, layout.ok);
            HashValue(hash, layout.rhw);
            HashValue(hash, layout.hasNormalSemantic);
            HashValue(hash, layout.hasBlendIndices);
            HashValue(hash, layout.hasBlendWeight);
            HashValue(hash, layout.hasPosition1);
            HashValue(hash, layout.hasPosition2);
            HashValue(hash, layout.hasPosition3);
            HashValue(hash, layout.hasPatchLikePositionSet);
            HashValue(hash, layout.positionOffset);
            HashValue(hash, layout.normalOffset);
            HashValue(hash, layout.normalType);
            HashValue(hash, layout.blendIndexOffset);
            HashValue(hash, layout.blendIndexType);
            HashValue(hash, layout.blendIndexComponents);
            HashValue(hash, layout.blendWeightOffset);
            HashValue(hash, layout.blendWeightType);
            HashValue(hash, layout.blendWeightComponents);
            HashValue(hash, layout.diffuseOffset);
            HashValue(hash, layout.texcoordOffset);
            HashValue(hash, layout.texcoordComponents);
            HashValue(hash, layout.elementCount);
            for (UINT i = 0; i < layout.elementCount; ++i) {
                const D3DVERTEXELEMENT9& element = layout.elements[i];
                HashValue(hash, element.Stream);
                HashValue(hash, element.Offset);
                HashValue(hash, element.Type);
                HashValue(hash, element.Method);
                HashValue(hash, element.Usage);
                HashValue(hash, element.UsageIndex);
            }

            return hash;
        }

        void LogRepackLayout(const RepackLayout& layout, std::uint16_t expectedStride, bool hasTexture)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            const std::uint32_t hash = HashRepackLayout(layout, expectedStride, hasTexture);
            for (std::size_t i = 0; i < g_loggedRepackCount; ++i) {
                if (g_loggedRepackHashes[i] == hash) {
                    return;
                }
            }

            if (g_loggedRepackCount >= g_loggedRepackHashes.size()) {
                return;
            }

            g_loggedRepackHashes[g_loggedRepackCount++] = hash;

            char elementText[768]{};
            std::size_t used = 0;
            for (UINT i = 0; i < layout.elementCount; ++i) {
                const D3DVERTEXELEMENT9& element = layout.elements[i];
                AppendText(
                    elementText,
                    sizeof(elementText),
                    used,
                    "%s{s=%u off=%u type=%u method=%u usage=%u idx=%u}",
                    i == 0 ? "" : " ",
                    element.Stream,
                    element.Offset,
                    element.Type,
                    element.Method,
                    element.Usage,
                    element.UsageIndex);
            }

            LogAlways(
                "repack %s stride=%u hasTexture=%d rhw=%d pos=%d normal=%d normalSemantic=%d diffuse=%d tex=%d texComp=%d blend=%d patchLike=%d reason=\"%s\" elements=%u %s",
                layout.ok ? "ok" : "fail",
                expectedStride,
                hasTexture ? 1 : 0,
                layout.rhw ? 1 : 0,
                layout.positionOffset,
                layout.normalOffset,
                layout.hasNormalSemantic ? 1 : 0,
                layout.diffuseOffset,
                layout.texcoordOffset,
                layout.texcoordComponents,
                (layout.hasBlendIndices || layout.hasBlendWeight) ? 1 : 0,
                layout.hasPatchLikePositionSet ? 1 : 0,
                layout.reason[0] ? layout.reason : "(none)",
                layout.elementCount,
                elementText[0] ? elementText : "(none)");
        }

        UINT PrimitiveVertexCount(std::uint16_t primitiveType, std::uint16_t primitiveCount)
        {
            switch (static_cast<D3DPRIMITIVETYPE>(primitiveType)) {
                case D3DPT_POINTLIST: return primitiveCount;
                case D3DPT_LINELIST: return static_cast<UINT>(primitiveCount) * 2;
                case D3DPT_LINESTRIP: return static_cast<UINT>(primitiveCount) + 1;
                case D3DPT_TRIANGLELIST: return static_cast<UINT>(primitiveCount) * 3;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN: return static_cast<UINT>(primitiveCount) + 2;
                default: return 0;
            }
        }

        bool ValidateIndexedDrawRange(
            IDirect3DDevice9* device,
            const PrimitiveDraw* draw,
            UINT repackedVertexCount,
            char* reason,
            std::size_t reasonSize)
        {
            if (!draw || !draw->indexBuffer) {
                SetMatrixReason(reason, reasonSize, "not indexed");
                return true;
            }

            IDirect3DIndexBuffer9* indexBuffer = nullptr;
            HRESULT hr = device ? device->GetIndices(&indexBuffer) : D3DERR_INVALIDCALL;
            if (FAILED(hr) || !indexBuffer) {
                SetMatrixReason(reason, reasonSize, "GetIndices failed hr=0x%08lx", static_cast<unsigned long>(hr));
                if (indexBuffer) {
                    indexBuffer->Release();
                }
                return false;
            }

            D3DINDEXBUFFER_DESC desc{};
            hr = indexBuffer->GetDesc(&desc);
            if (FAILED(hr)) {
                indexBuffer->Release();
                SetMatrixReason(reason, reasonSize, "GetDesc failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            const UINT indexSize =
                desc.Format == D3DFMT_INDEX16 ? 2u :
                desc.Format == D3DFMT_INDEX32 ? 4u : 0u;
            if (indexSize == 0) {
                indexBuffer->Release();
                SetMatrixReason(reason, reasonSize, "unsupported index format=%u", desc.Format);
                return false;
            }

            const UINT indexCount = PrimitiveVertexCount(draw->primitiveType, draw->primitiveCount);
            const UINT availableIndices = desc.Size / indexSize;
            if (indexCount == 0 || draw->startIndex > availableIndices || indexCount > availableIndices - draw->startIndex) {
                indexBuffer->Release();
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "index span out of buffer start=%u count=%u available=%u",
                    draw->startIndex,
                    indexCount,
                    availableIndices);
                return false;
            }

            const UINT lockOffset = draw->startIndex * indexSize;
            const UINT lockSize = indexCount * indexSize;
            void* data = nullptr;
            hr = indexBuffer->Lock(lockOffset, lockSize, &data, D3DLOCK_READONLY);
            if (FAILED(hr)) {
                hr = indexBuffer->Lock(lockOffset, lockSize, &data, 0);
            }
            if (FAILED(hr) || !data) {
                indexBuffer->Release();
                SetMatrixReason(reason, reasonSize, "index Lock failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }

            UINT minIndex = 0xFFFFFFFFu;
            UINT maxIndex = 0u;
            if (indexSize == 2) {
                const auto* indices = static_cast<const std::uint16_t*>(data);
                for (UINT i = 0; i < indexCount; ++i) {
                    minIndex = indices[i] < minIndex ? indices[i] : minIndex;
                    maxIndex = indices[i] > maxIndex ? indices[i] : maxIndex;
                }
            } else {
                const auto* indices = static_cast<const std::uint32_t*>(data);
                for (UINT i = 0; i < indexCount; ++i) {
                    minIndex = indices[i] < minIndex ? indices[i] : minIndex;
                    maxIndex = indices[i] > maxIndex ? indices[i] : maxIndex;
                }
            }

            indexBuffer->Unlock();
            indexBuffer->Release();

            if (maxIndex >= repackedVertexCount) {
                SetMatrixReason(
                    reason,
                    reasonSize,
                    "maxIndex=%u repackedVertices=%u minIndex=%u start=%u count=%u",
                    maxIndex,
                    repackedVertexCount,
                    minIndex,
                    draw->startIndex,
                    indexCount);
                return false;
            }

            SetMatrixReason(
                reason,
                reasonSize,
                "index range ok min=%u max=%u vertices=%u count=%u",
                minIndex,
                maxIndex,
                repackedVertexCount,
                indexCount);
            return true;
        }

        bool HasStage0Texture(IDirect3DDevice9* device)
        {
            IDirect3DBaseTexture9* texture = nullptr;
            const bool hasTexture = SUCCEEDED(device->GetTexture(0, &texture)) && texture != nullptr;
            if (texture) {
                texture->Release();
            }

            return hasTexture;
        }

        Stage0TextureInfo QueryStage0TextureInfo(IDirect3DDevice9* device)
        {
            Stage0TextureInfo info{};
            if (!device) {
                return info;
            }

            IDirect3DBaseTexture9* baseTexture = nullptr;
            if (FAILED(device->GetTexture(0, &baseTexture)) || !baseTexture) {
                return info;
            }

            info.hasTexture = true;
            if (baseTexture->GetType() == D3DRTYPE_TEXTURE) {
                auto* texture = static_cast<IDirect3DTexture9*>(baseTexture);
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(texture->GetLevelDesc(0, &desc))) {
                    info.width = desc.Width;
                    info.height = desc.Height;
                    info.usage = desc.Usage;
                    info.pool = desc.Pool;
                    info.format = desc.Format;
                    info.renderTarget = (desc.Usage & D3DUSAGE_RENDERTARGET) != 0;

                    D3DVIEWPORT9 viewport{};
                    if (SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width != 0 && viewport.Height != 0) {
                        info.viewportSized =
                            desc.Width >= viewport.Width - 4 && desc.Width <= viewport.Width + 4 &&
                            desc.Height >= viewport.Height - 4 && desc.Height <= viewport.Height + 4;
                    }

                    IDirect3DSurface9* renderTarget = nullptr;
                    if (SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget) {
                        D3DSURFACE_DESC rtDesc{};
                        if (SUCCEEDED(renderTarget->GetDesc(&rtDesc))) {
                            info.backbufferSized =
                                desc.Width >= rtDesc.Width - 4 && desc.Width <= rtDesc.Width + 4 &&
                                desc.Height >= rtDesc.Height - 4 && desc.Height <= rtDesc.Height + 4;
                        }
                        renderTarget->Release();
                    }

                    info.screenSized = info.renderTarget || info.viewportSized || info.backbufferSized;
                }
            }

            baseTexture->Release();
            return info;
        }

        void CapturePassStateSnapshot(IDirect3DDevice9* device, FixedFunctionPassResult& result)
        {
            if (!device) {
                return;
            }

            device->GetRenderState(D3DRS_COLORWRITEENABLE, &result.colorWriteMask);
            device->GetRenderState(D3DRS_ZENABLE, &result.zEnable);
            device->GetRenderState(D3DRS_ZWRITEENABLE, &result.zWriteEnable);
            device->GetRenderState(D3DRS_ALPHABLENDENABLE, &result.alphaBlendEnable);
            device->GetRenderState(D3DRS_SRCBLEND, &result.srcBlend);
            device->GetRenderState(D3DRS_DESTBLEND, &result.dstBlend);
            result.renderStateSnapshotValid = true;
        }

        void CopyTextureInfoToResult(FixedFunctionPassResult& result, const Stage0TextureInfo& textureInfo)
        {
            result.hasTexture = textureInfo.hasTexture;
            result.textureWidth = textureInfo.width;
            result.textureHeight = textureInfo.height;
            result.textureUsage = textureInfo.usage;
            result.textureScreenSized = textureInfo.screenSized;
            result.textureRenderTarget = textureInfo.renderTarget;
        }

        void SetSkippedProxyResult(FixedFunctionPassResult& result, FixedFunctionPassReason reason, const char* detail)
        {
            result.status = FixedFunctionPassStatus::SkippedUi;
            result.hr = S_OK;
            result.reason = reason;
            SetPassDetail(result, "%s", detail && detail[0] ? detail : FixedFunctionPassReasonName(reason));
        }

        void CountProxySkipReason(FixedFunctionPassReason reason)
        {
            switch (reason) {
                case FixedFunctionPassReason::SkippedSkinned:
                    ++g_counters.proxySkippedSkinned;
                    break;
                case FixedFunctionPassReason::SkippedPatch:
                    ++g_counters.proxySkippedPatch;
                    break;
                case FixedFunctionPassReason::SkippedTransparent:
                    ++g_counters.proxySkippedTransparent;
                    break;
                case FixedFunctionPassReason::SkippedScreenCopy:
                    ++g_counters.proxySkippedScreenCopy;
                    break;
                case FixedFunctionPassReason::SkippedCameraMismatch:
                    ++g_counters.proxySkippedCameraMismatch;
                    break;
                case FixedFunctionPassReason::SkippedScreenPlane:
                    ++g_counters.proxySkippedScreenPlane;
                    break;
                case FixedFunctionPassReason::SkippedProjectedOversize:
                    ++g_counters.proxySkippedProjectedOversize;
                    break;
                case FixedFunctionPassReason::MissingTexcoord:
                    ++g_counters.proxySkippedNoTexcoord;
                    break;
                default:
                    break;
            }
        }

        bool SolidWorldRenderStateShouldSkip(IDirect3DDevice9* device, FixedFunctionPassReason& reason, char* detail, std::size_t detailSize)
        {
            if (!device || g_config.proxyGeometryFilter != ProxyGeometryFilterMode::SolidWorldOnly) {
                return false;
            }

            DWORD colorWrite = D3DCOLORWRITEENABLE_RED |
                D3DCOLORWRITEENABLE_GREEN |
                D3DCOLORWRITEENABLE_BLUE |
                D3DCOLORWRITEENABLE_ALPHA;
            if (SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite)) && colorWrite == 0) {
                reason = FixedFunctionPassReason::SkippedTransparent;
                snprintf(detail, detailSize, "solid-world filter rejected color-write disabled draw");
                return true;
            }

            DWORD alphaBlend = FALSE;
            if (SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) && alphaBlend != FALSE) {
                DWORD srcBlend = 0;
                DWORD dstBlend = 0;
                device->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
                device->GetRenderState(D3DRS_DESTBLEND, &dstBlend);
                reason = FixedFunctionPassReason::SkippedTransparent;
                snprintf(
                    detail,
                    detailSize,
                    "solid-world filter rejected alpha blended draw src=%lu dst=%lu",
                    static_cast<unsigned long>(srcBlend),
                    static_cast<unsigned long>(dstBlend));
                return true;
            }

            DWORD zWrite = TRUE;
            if (SUCCEEDED(device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) && zWrite == FALSE) {
                reason = FixedFunctionPassReason::SkippedTransparent;
                snprintf(detail, detailSize, "solid-world filter rejected ZWRITE disabled draw");
                return true;
            }

            return false;
        }

        bool Stage0TextureLooksLikeScreenCopy(IDirect3DDevice9* device, char* detail, std::size_t detailSize)
        {
            if (!device) {
                return false;
            }

            IDirect3DBaseTexture9* baseTexture = nullptr;
            if (FAILED(device->GetTexture(0, &baseTexture)) || !baseTexture) {
                return false;
            }

            if (baseTexture->GetType() != D3DRTYPE_TEXTURE) {
                baseTexture->Release();
                return false;
            }

            auto* texture = static_cast<IDirect3DTexture9*>(baseTexture);
            D3DSURFACE_DESC desc{};
            const HRESULT descHr = texture->GetLevelDesc(0, &desc);
            baseTexture->Release();
            if (FAILED(descHr)) {
                return false;
            }

            D3DVIEWPORT9 viewport{};
            const bool viewportValid = SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width != 0 && viewport.Height != 0;
            const bool viewportSized =
                viewportValid &&
                desc.Width >= viewport.Width - 4 && desc.Width <= viewport.Width + 4 &&
                desc.Height >= viewport.Height - 4 && desc.Height <= viewport.Height + 4;

            bool backbufferSized = false;
            IDirect3DSurface9* renderTarget = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(0, &renderTarget)) && renderTarget) {
                D3DSURFACE_DESC rtDesc{};
                if (SUCCEEDED(renderTarget->GetDesc(&rtDesc))) {
                    backbufferSized =
                        desc.Width >= rtDesc.Width - 4 && desc.Width <= rtDesc.Width + 4 &&
                        desc.Height >= rtDesc.Height - 4 && desc.Height <= rtDesc.Height + 4;
                }
                renderTarget->Release();
            }

            const bool renderTargetTexture = (desc.Usage & D3DUSAGE_RENDERTARGET) != 0;
            const bool defaultPoolFullscreen = desc.Pool == D3DPOOL_DEFAULT && (viewportSized || backbufferSized);
            if (renderTargetTexture || defaultPoolFullscreen) {
                snprintf(
                    detail,
                    detailSize,
                    "solid-world filter rejected screen/render-target texture %ux%u usage=0x%08lx pool=%u viewportSized=%d backbufferSized=%d",
                    desc.Width,
                    desc.Height,
                    static_cast<unsigned long>(desc.Usage),
                    static_cast<unsigned int>(desc.Pool),
                    viewportSized ? 1 : 0,
                    backbufferSized ? 1 : 0);
                return true;
            }

            return false;
        }

        bool SolidWorldLayoutShouldSkip(
            const RepackLayout& layout,
            bool hasTexture,
            const Stage0TextureInfo& textureInfo,
            FixedFunctionPassReason& reason,
            char* detail,
            std::size_t detailSize)
        {
            if (g_config.proxyGeometryFilter != ProxyGeometryFilterMode::SolidWorldOnly) {
                return false;
            }

            const bool skinnedLayout = layout.hasBlendIndices || layout.hasBlendWeight;
            if (skinnedLayout && g_config.proxySkinnedMode != ProxySkinnedMode::PaletteSkinning) {
                reason = FixedFunctionPassReason::SkippedSkinned;
                snprintf(
                    detail,
                    detailSize,
                    "solid-world filter rejected skinned declaration blendIndices=%d blendWeight=%d",
                    layout.hasBlendIndices ? 1 : 0,
                    layout.hasBlendWeight ? 1 : 0);
                return true;
            }

            if (skinnedLayout &&
                (layout.blendIndexOffset < 0 || layout.blendWeightOffset < 0 ||
                 layout.blendIndexComponents < 2 || layout.blendWeightComponents < 1)) {
                reason = FixedFunctionPassReason::SkippedSkinned;
                snprintf(detail, detailSize,
                    "palette skinning metadata incomplete indices=%d/%d weights=%d/%d",
                    layout.blendIndexOffset, layout.blendIndexComponents,
                    layout.blendWeightOffset, layout.blendWeightComponents);
                return true;
            }

            if (layout.hasPatchLikePositionSet && !skinnedLayout) {
                reason = FixedFunctionPassReason::SkippedPatch;
                snprintf(
                    detail,
                    detailSize,
                    "solid-world filter rejected patch/control declaration position1=%d position2=%d position3=%d",
                    layout.hasPosition1 ? 1 : 0,
                    layout.hasPosition2 ? 1 : 0,
                    layout.hasPosition3 ? 1 : 0);
                return true;
            }

            if (!layout.hasNormalSemantic) {
                if (textureInfo.screenSized) {
                    reason = FixedFunctionPassReason::SkippedScreenCopy;
                    snprintf(
                        detail,
                        detailSize,
                        "solid-world filter rejected screen-sized texture without NORMAL0 tex=%ux%u usage=0x%08lx",
                        textureInfo.width,
                        textureInfo.height,
                        static_cast<unsigned long>(textureInfo.usage));
                    return true;
                }
                reason = FixedFunctionPassReason::SkippedPatch;
                snprintf(detail, detailSize, "solid-world filter rejected declaration without NORMAL0 semantic");
                return true;
            }

            if (!layout.hasTexcoord) {
                if (textureInfo.screenSized) {
                    reason = FixedFunctionPassReason::SkippedScreenCopy;
                    snprintf(
                        detail,
                        detailSize,
                        "solid-world filter rejected screen-sized texture without TEXCOORD0 tex=%ux%u usage=0x%08lx",
                        textureInfo.width,
                        textureInfo.height,
                        static_cast<unsigned long>(textureInfo.usage));
                    return true;
                }
                reason = FixedFunctionPassReason::MissingTexcoord;
                snprintf(detail, detailSize, "solid-world filter rejected declaration without TEXCOORD0");
                return true;
            }

            if (!hasTexture && !g_config.captureAllowTexturelessProxy) {
                reason = FixedFunctionPassReason::MissingTexcoord;
                snprintf(detail, detailSize, "solid-world filter rejected textureless proxy draw");
                return true;
            }

            return false;
        }

        bool ValidateOrLockFrameCamera(
            IDirect3DDevice9* device,
            const FixedFunctionTransformSet& transforms,
            char* detail,
            std::size_t detailSize)
        {
            if (!device ||
                g_config.proxyGeometryFilter != ProxyGeometryFilterMode::SolidWorldOnly ||
                !transforms.valid ||
                !transforms.setView ||
                !transforms.setProjection) {
                return true;
            }

            D3DVIEWPORT9 viewport{};
            if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0) {
                return true;
            }

            char matrixReason[96]{};
            if (!MatrixValuesAreFinite(transforms.view, matrixReason, sizeof(matrixReason)) ||
                !MatrixValuesAreFinite(transforms.projection, matrixReason, sizeof(matrixReason)) ||
                MatrixNearlyIdentity(transforms.projection, 1.0e-4f) ||
                LooksLikeScreenMatrix(transforms.projection, &viewport, matrixReason, sizeof(matrixReason))) {
                ++g_counters.cameraCandidateRejected;
                snprintf(detail, detailSize, "invalid camera candidate: %s", matrixReason[0] ? matrixReason : "identity projection");
                return false;
            }

            const std::size_t limit = g_config.proxyCameraLockMode == ProxyCameraLockMode::Single
                ? 1
                : g_frameCameraCandidates.size();
            for (std::size_t i = 0; i < g_frameCameraCandidateCount; ++i) {
                FrameCameraLock& candidate = g_frameCameraCandidates[i];
                const D3DVIEWPORT9& locked = candidate.viewport;
                const bool viewportMatches =
                    viewport.X == locked.X &&
                    viewport.Y == locked.Y &&
                    viewport.Width == locked.Width &&
                    viewport.Height == locked.Height &&
                    AbsFloat(viewport.MinZ - locked.MinZ) <= 1.0e-5f &&
                    AbsFloat(viewport.MaxZ - locked.MaxZ) <= 1.0e-5f;
                if (viewportMatches &&
                    MatrixNearlyEqual(transforms.view, candidate.view, 1.0e-3f) &&
                    MatrixNearlyEqual(transforms.projection, candidate.projection, 1.0e-3f)) {
                    ++candidate.hits;
                    return true;
                }
            }

            if (g_frameCameraCandidateCount < limit) {
                FrameCameraLock& candidate = g_frameCameraCandidates[g_frameCameraCandidateCount++];
                candidate.valid = true;
                candidate.view = transforms.view;
                candidate.projection = transforms.projection;
                candidate.viewport = viewport;
                candidate.hits = 1;
                ++g_counters.cameraCandidateAccepted;
                if (g_config.debugLog || g_counters.proxySubmitted < 24) {
                    LogAlways(
                        "proxy camera candidate accepted index=%u/%u viewport=%lux%lu xy=(%lu,%lu) assembly=%s",
                        static_cast<unsigned int>(g_frameCameraCandidateCount - 1),
                        static_cast<unsigned int>(limit),
                        static_cast<unsigned long>(viewport.Width),
                        static_cast<unsigned long>(viewport.Height),
                        static_cast<unsigned long>(viewport.X),
                        static_cast<unsigned long>(viewport.Y),
                        TransformAssemblyModeName(g_config.transformAssemblyMode));
                }
                return true;
            }

            ++g_counters.cameraCandidateRejected;
            snprintf(
                detail,
                detailSize,
                "proxy camera candidate cache full count=%u viewport=%lux%lu/%lu,%lu",
                static_cast<unsigned int>(limit),
                static_cast<unsigned long>(viewport.Width),
                static_cast<unsigned long>(viewport.Height),
                static_cast<unsigned long>(viewport.X),
                static_cast<unsigned long>(viewport.Y));
            return false;
        }

        void ReleaseScratchVertexBuffer()
        {
            if (g_scratchVertexBuffer) {
                g_scratchVertexBuffer->Release();
                g_scratchVertexBuffer = nullptr;
            }
            g_scratchDevice = nullptr;
            g_scratchVertexCapacity = 0;
        }

        bool EnsureScratchVertexBuffer(IDirect3DDevice9* device, UINT vertexCount)
        {
            if (!device || vertexCount == 0) {
                return false;
            }

            if (g_scratchDevice != device) {
                ReleaseScratchVertexBuffer();
                g_scratchDevice = device;
            }

            if (g_scratchVertexBuffer && g_scratchVertexCapacity >= vertexCount) {
                return true;
            }

            if (g_scratchVertexBuffer) {
                g_scratchVertexBuffer->Release();
                g_scratchVertexBuffer = nullptr;
                g_scratchVertexCapacity = 0;
            }

            const HRESULT hr = device->CreateVertexBuffer(
                vertexCount * sizeof(ScratchVertex),
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                SCRATCH_VERTEX_FVF,
                D3DPOOL_DEFAULT,
                &g_scratchVertexBuffer,
                nullptr);

            if (FAILED(hr)) {
                LogAlways("repack scratch CreateVertexBuffer failed vertices=%u hr=0x%08lx", vertexCount, static_cast<unsigned long>(hr));
                return false;
            }

            g_scratchVertexCapacity = vertexCount;
            return true;
        }

        bool ReadFloat3(const std::uint8_t* vertex, int offset, float& x, float& y, float& z)
        {
            const float* values = reinterpret_cast<const float*>(vertex + offset);
            x = values[0];
            y = values[1];
            z = values[2];
            return IsFiniteFloat(x) && IsFiniteFloat(y) && IsFiniteFloat(z);
        }

        bool ReadNormalizedFloat3(const std::uint8_t* vertex, int offset, float& x, float& y, float& z)
        {
            if (!ReadFloat3(vertex, offset, x, y, z)) {
                return false;
            }

            return Normalize3(x, y, z);
        }

        bool AccumulateTriangleNormal(ScratchVertex* vertices, UINT vertexCount, UINT i0, UINT i1, UINT i2)
        {
            if (!vertices || i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
                return false;
            }

            ScratchVertex& v0 = vertices[i0];
            ScratchVertex& v1 = vertices[i1];
            ScratchVertex& v2 = vertices[i2];
            const float ax = v1.x - v0.x;
            const float ay = v1.y - v0.y;
            const float az = v1.z - v0.z;
            const float bx = v2.x - v0.x;
            const float by = v2.y - v0.y;
            const float bz = v2.z - v0.z;
            float nx = 0.0f;
            float ny = 0.0f;
            float nz = 0.0f;
            Cross3(ax, ay, az, bx, by, bz, nx, ny, nz);
            if (!Normalize3(nx, ny, nz)) {
                return false;
            }

            v0.nx += nx;
            v0.ny += ny;
            v0.nz += nz;
            v1.nx += nx;
            v1.ny += ny;
            v1.nz += nz;
            v2.nx += nx;
            v2.ny += ny;
            v2.nz += nz;
            return true;
        }

        void NormalizeScratchNormals(ScratchVertex* vertices, UINT vertexCount)
        {
            if (!vertices) {
                return;
            }

            for (UINT i = 0; i < vertexCount; ++i) {
                if (!Normalize3(vertices[i].nx, vertices[i].ny, vertices[i].nz)) {
                    vertices[i].nx = 0.0f;
                    vertices[i].ny = 1.0f;
                    vertices[i].nz = 0.0f;
                }
            }
        }

        UINT GenerateUnindexedFlatNormals(ScratchVertex* vertices, UINT vertexCount, const PrimitiveDraw* draw)
        {
            if (!vertices || !draw) {
                return 0;
            }

            UINT generated = 0;
            switch (static_cast<D3DPRIMITIVETYPE>(draw->primitiveType)) {
                case D3DPT_TRIANGLELIST:
                    for (UINT i = 0; i + 2 < vertexCount; i += 3) {
                        generated += AccumulateTriangleNormal(vertices, vertexCount, i, i + 1, i + 2) ? 1u : 0u;
                    }
                    break;

                case D3DPT_TRIANGLESTRIP:
                    for (UINT i = 0; i < draw->primitiveCount && i + 2 < vertexCount; ++i) {
                        if ((i & 1u) == 0) {
                            generated += AccumulateTriangleNormal(vertices, vertexCount, i, i + 1, i + 2) ? 1u : 0u;
                        } else {
                            generated += AccumulateTriangleNormal(vertices, vertexCount, i + 1, i, i + 2) ? 1u : 0u;
                        }
                    }
                    break;

                case D3DPT_TRIANGLEFAN:
                    for (UINT i = 0; i < draw->primitiveCount && i + 2 < vertexCount; ++i) {
                        generated += AccumulateTriangleNormal(vertices, vertexCount, 0, i + 1, i + 2) ? 1u : 0u;
                    }
                    break;

                default:
                    break;
            }

            if (generated != 0) {
                NormalizeScratchNormals(vertices, vertexCount);
            }
            return generated;
        }

        UINT GenerateIndexedFlatNormals(IDirect3DDevice9* device, ScratchVertex* vertices, UINT vertexCount, const PrimitiveDraw* draw)
        {
            if (!device || !vertices || !draw || !draw->indexBuffer) {
                return 0;
            }

            IDirect3DIndexBuffer9* indexBuffer = nullptr;
            HRESULT hr = device->GetIndices(&indexBuffer);
            if (FAILED(hr) || !indexBuffer) {
                if (indexBuffer) {
                    indexBuffer->Release();
                }
                return 0;
            }

            D3DINDEXBUFFER_DESC desc{};
            hr = indexBuffer->GetDesc(&desc);
            if (FAILED(hr)) {
                indexBuffer->Release();
                return 0;
            }

            const UINT indexSize =
                desc.Format == D3DFMT_INDEX16 ? 2u :
                desc.Format == D3DFMT_INDEX32 ? 4u : 0u;
            const UINT indexCount = PrimitiveVertexCount(draw->primitiveType, draw->primitiveCount);
            const UINT availableIndices = indexSize != 0 ? desc.Size / indexSize : 0u;
            if (indexSize == 0 || indexCount == 0 || draw->startIndex > availableIndices ||
                indexCount > availableIndices - draw->startIndex) {
                indexBuffer->Release();
                return 0;
            }

            void* data = nullptr;
            hr = indexBuffer->Lock(draw->startIndex * indexSize, indexCount * indexSize, &data, D3DLOCK_READONLY);
            if (FAILED(hr)) {
                hr = indexBuffer->Lock(draw->startIndex * indexSize, indexCount * indexSize, &data, 0);
            }
            if (FAILED(hr) || !data) {
                indexBuffer->Release();
                return 0;
            }

            auto indexAt = [&](UINT index) -> UINT {
                if (indexSize == 2) {
                    return static_cast<const std::uint16_t*>(data)[index];
                }
                return static_cast<const std::uint32_t*>(data)[index];
            };

            UINT generated = 0;
            switch (static_cast<D3DPRIMITIVETYPE>(draw->primitiveType)) {
                case D3DPT_TRIANGLELIST:
                    for (UINT i = 0; i + 2 < indexCount; i += 3) {
                        generated += AccumulateTriangleNormal(vertices, vertexCount, indexAt(i), indexAt(i + 1), indexAt(i + 2)) ? 1u : 0u;
                    }
                    break;

                case D3DPT_TRIANGLESTRIP:
                    for (UINT i = 0; i < draw->primitiveCount && i + 2 < indexCount; ++i) {
                        if ((i & 1u) == 0) {
                            generated += AccumulateTriangleNormal(vertices, vertexCount, indexAt(i), indexAt(i + 1), indexAt(i + 2)) ? 1u : 0u;
                        } else {
                            generated += AccumulateTriangleNormal(vertices, vertexCount, indexAt(i + 1), indexAt(i), indexAt(i + 2)) ? 1u : 0u;
                        }
                    }
                    break;

                case D3DPT_TRIANGLEFAN:
                    for (UINT i = 0; i < draw->primitiveCount && i + 2 < indexCount; ++i) {
                        generated += AccumulateTriangleNormal(vertices, vertexCount, indexAt(0), indexAt(i + 1), indexAt(i + 2)) ? 1u : 0u;
                    }
                    break;

                default:
                    break;
            }

            indexBuffer->Unlock();
            indexBuffer->Release();

            if (generated != 0) {
                NormalizeScratchNormals(vertices, vertexCount);
            }
            return generated;
        }

        void ExpandBounds(VertexBounds& bounds, float x, float y, float z)
        {
            if (!bounds.valid) {
                bounds.valid = true;
                bounds.minX = bounds.maxX = x;
                bounds.minY = bounds.maxY = y;
                bounds.minZ = bounds.maxZ = z;
                return;
            }

            bounds.minX = MinFloat(bounds.minX, x);
            bounds.minY = MinFloat(bounds.minY, y);
            bounds.minZ = MinFloat(bounds.minZ, z);
            bounds.maxX = MaxFloat(bounds.maxX, x);
            bounds.maxY = MaxFloat(bounds.maxY, y);
            bounds.maxZ = MaxFloat(bounds.maxZ, z);
        }

        bool ValidateVertexBounds(const VertexBounds& bounds, char* reason, std::size_t reasonSize)
        {
            if (!bounds.valid) {
                snprintf(reason, reasonSize, "no finite positions");
                return false;
            }

            const float extentX = bounds.maxX - bounds.minX;
            const float extentY = bounds.maxY - bounds.minY;
            const float extentZ = bounds.maxZ - bounds.minZ;
            const float maxAbs =
                MaxFloat(MaxFloat(AbsFloat(bounds.minX), AbsFloat(bounds.maxX)),
                    MaxFloat(
                        MaxFloat(AbsFloat(bounds.minY), AbsFloat(bounds.maxY)),
                        MaxFloat(AbsFloat(bounds.minZ), AbsFloat(bounds.maxZ))));
            const float maxExtent = MaxFloat(extentX, MaxFloat(extentY, extentZ));

            if (!IsFiniteFloat(maxAbs) || !IsFiniteFloat(maxExtent) || maxAbs > 1.0e6f || maxExtent > 1.0e6f) {
                snprintf(reason, reasonSize, "bounds too large maxAbs=%g maxExtent=%g", maxAbs, maxExtent);
                return false;
            }

            if (maxExtent < 1.0e-6f) {
                snprintf(reason, reasonSize, "degenerate bounds extent=%g", maxExtent);
                return false;
            }

            snprintf(
                reason,
                reasonSize,
                "bounds ok min=(%g,%g,%g) max=(%g,%g,%g)",
                bounds.minX,
                bounds.minY,
                bounds.minZ,
                bounds.maxX,
                bounds.maxY,
                bounds.maxZ);
            return true;
        }

        bool BuildWvpFromTransformSet(const FixedFunctionTransformSet* transforms, D3DMATRIX& out)
        {
            if (!transforms || !transforms->valid ||
                !transforms->setWorld || !transforms->setView || !transforms->setProjection) {
                return false;
            }

            out = MultiplyMatrix(MultiplyMatrix(transforms->world, transforms->view), transforms->projection);
            return true;
        }

        bool EvaluateProjectedBoundsMetrics(
            const D3DMATRIX& wvp,
            const VertexBounds& bounds,
            ProjectedBoundsMetrics& metrics,
            char* reason,
            std::size_t reasonSize)
        {
            if (!bounds.valid) {
                snprintf(reason, reasonSize, "bounds invalid");
                return false;
            }

            if (!MatrixValuesAreFinite(wvp, reason, reasonSize)) {
                return false;
            }

            const float corners[8][3] = {
                { bounds.minX, bounds.minY, bounds.minZ },
                { bounds.maxX, bounds.minY, bounds.minZ },
                { bounds.minX, bounds.maxY, bounds.minZ },
                { bounds.maxX, bounds.maxY, bounds.minZ },
                { bounds.minX, bounds.minY, bounds.maxZ },
                { bounds.maxX, bounds.minY, bounds.maxZ },
                { bounds.minX, bounds.maxY, bounds.maxZ },
                { bounds.maxX, bounds.maxY, bounds.maxZ },
            };

            metrics.minNdcX = 1.0e30f;
            metrics.minNdcY = 1.0e30f;
            metrics.maxNdcX = -1.0e30f;
            metrics.maxNdcY = -1.0e30f;
            metrics.maxAbsNdc = 0.0f;

            for (const auto& corner : corners) {
                float clipX = 0.0f;
                float clipY = 0.0f;
                float clipZ = 0.0f;
                float clipW = 0.0f;
                if (!TransformPointRowMajorFull(wvp, corner[0], corner[1], corner[2], clipX, clipY, clipZ, clipW)) {
                    continue;
                }

                const float invW = 1.0f / clipW;
                const float ndcX = clipX * invW;
                const float ndcY = clipY * invW;
                const float ndcZ = clipZ * invW;
                if (!IsFiniteFloat(ndcX) || !IsFiniteFloat(ndcY) || !IsFiniteFloat(ndcZ)) {
                    continue;
                }

                ++metrics.finiteCorners;
                metrics.minNdcX = MinFloat(metrics.minNdcX, ndcX);
                metrics.minNdcY = MinFloat(metrics.minNdcY, ndcY);
                metrics.maxNdcX = MaxFloat(metrics.maxNdcX, ndcX);
                metrics.maxNdcY = MaxFloat(metrics.maxNdcY, ndcY);
                metrics.maxAbsNdc = MaxFloat(metrics.maxAbsNdc, MaxFloat(MaxFloat(AbsFloat(ndcX), AbsFloat(ndcY)), AbsFloat(ndcZ)));
            }

            if (metrics.finiteCorners < 4) {
                snprintf(reason, reasonSize, "projected bounds finite corners=%d", metrics.finiteCorners);
                return false;
            }

            metrics.extentX = metrics.maxNdcX - metrics.minNdcX;
            metrics.extentY = metrics.maxNdcY - metrics.minNdcY;
            metrics.area = metrics.extentX * metrics.extentY;
            if (!IsFiniteFloat(metrics.extentX) || !IsFiniteFloat(metrics.extentY) ||
                !IsFiniteFloat(metrics.area) || !IsFiniteFloat(metrics.maxAbsNdc)) {
                snprintf(reason, reasonSize, "projected bounds are non-finite");
                return false;
            }

            metrics.valid = true;
            snprintf(
                reason,
                reasonSize,
                "projected ndc=[%g,%g]-[%g,%g] extent=%gx%g maxNdc=%g finite=%d",
                metrics.minNdcX,
                metrics.minNdcY,
                metrics.maxNdcX,
                metrics.maxNdcY,
                metrics.extentX,
                metrics.extentY,
                metrics.maxAbsNdc,
                metrics.finiteCorners);
            return true;
        }

        bool IsNearFullscreenProjection(const ProjectedBoundsMetrics& metrics)
        {
            if (!metrics.valid) {
                return false;
            }

            const bool coversCenter =
                metrics.minNdcX <= -0.75f &&
                metrics.maxNdcX >= 0.75f &&
                metrics.minNdcY <= -0.75f &&
                metrics.maxNdcY >= 0.75f;
            const bool largeExtent = metrics.extentX >= 1.65f && metrics.extentY >= 1.65f;
            return coversCenter || largeExtent;
        }

        void LogProjectedProxyDecision(
            const char* tag,
            const PrimitiveDraw* draw,
            UINT vertexCount,
            const Stage0TextureInfo& textureInfo,
            const ProjectedBoundsMetrics& metrics,
            bool alphaTestEnabled,
            const char* reason)
        {
            if (!g_config.logFile && !g_config.debugLog) {
                return;
            }

            static std::uint32_t logged = 0;
            if (logged >= 64) {
                return;
            }
            ++logged;

            LogAlways(
                "proxy %s primitive=%u stride=%u vertices=%u primitiveCount=%u alphaTest=%d tex=%d texSize=%ux%u texUsage=0x%08lx screenSized=%d rt=%d ndc=[%g,%g]-[%g,%g] extent=%gx%g maxNdc=%g reason=\"%s\"",
                tag && tag[0] ? tag : "projection",
                draw ? draw->primitiveType : 0,
                draw ? draw->stride : 0,
                vertexCount,
                draw ? draw->primitiveCount : 0,
                alphaTestEnabled ? 1 : 0,
                textureInfo.hasTexture ? 1 : 0,
                textureInfo.width,
                textureInfo.height,
                static_cast<unsigned long>(textureInfo.usage),
                textureInfo.screenSized ? 1 : 0,
                textureInfo.renderTarget ? 1 : 0,
                metrics.minNdcX,
                metrics.minNdcY,
                metrics.maxNdcX,
                metrics.maxNdcY,
                metrics.extentX,
                metrics.extentY,
                metrics.maxAbsNdc,
                reason && reason[0] ? reason : "(none)");
        }

        bool TransformPointAffine(const D3DMATRIX& matrix, float x, float y, float z, float& outX, float& outY, float& outZ)
        {
            outX = x * matrix._11 + y * matrix._21 + z * matrix._31 + matrix._41;
            outY = x * matrix._12 + y * matrix._22 + z * matrix._32 + matrix._42;
            outZ = x * matrix._13 + y * matrix._23 + z * matrix._33 + matrix._43;
            return IsFiniteFloat(outX) && IsFiniteFloat(outY) && IsFiniteFloat(outZ);
        }

        bool EstimateCameraPositionFromView(const D3DMATRIX& view, float& outX, float& outY, float& outZ)
        {
            if (!MatrixValuesAreFinite(view, nullptr, 0)) {
                return false;
            }

            const float row0 = Length3(view._11, view._12, view._13);
            const float row1 = Length3(view._21, view._22, view._23);
            const float row2 = Length3(view._31, view._32, view._33);
            if (row0 < 0.25f || row1 < 0.25f || row2 < 0.25f ||
                row0 > 4.0f || row1 > 4.0f || row2 > 4.0f) {
                return false;
            }

            outX = -(view._41 * view._11 + view._42 * view._12 + view._43 * view._13);
            outY = -(view._41 * view._21 + view._42 * view._22 + view._43 * view._23);
            outZ = -(view._41 * view._31 + view._42 * view._32 + view._43 * view._33);
            return IsFiniteFloat(outX) && IsFiniteFloat(outY) && IsFiniteFloat(outZ) &&
                   MaxFloat(AbsFloat(outX), MaxFloat(AbsFloat(outY), AbsFloat(outZ))) < 1.0e6f;
        }

        bool EvaluateWorldBounds(
            const D3DMATRIX& world,
            const VertexBounds& bounds,
            float& centerX,
            float& centerY,
            float& centerZ,
            float& radius)
        {
            if (!bounds.valid || !MatrixValuesAreFinite(world, nullptr, 0)) {
                return false;
            }

            const float corners[8][3] = {
                { bounds.minX, bounds.minY, bounds.minZ },
                { bounds.maxX, bounds.minY, bounds.minZ },
                { bounds.minX, bounds.maxY, bounds.minZ },
                { bounds.maxX, bounds.maxY, bounds.minZ },
                { bounds.minX, bounds.minY, bounds.maxZ },
                { bounds.maxX, bounds.minY, bounds.maxZ },
                { bounds.minX, bounds.maxY, bounds.maxZ },
                { bounds.maxX, bounds.maxY, bounds.maxZ },
            };

            float minX = 1.0e30f;
            float minY = 1.0e30f;
            float minZ = 1.0e30f;
            float maxX = -1.0e30f;
            float maxY = -1.0e30f;
            float maxZ = -1.0e30f;
            int finiteCorners = 0;
            for (const auto& corner : corners) {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                if (!TransformPointAffine(world, corner[0], corner[1], corner[2], x, y, z)) {
                    continue;
                }

                minX = MinFloat(minX, x);
                minY = MinFloat(minY, y);
                minZ = MinFloat(minZ, z);
                maxX = MaxFloat(maxX, x);
                maxY = MaxFloat(maxY, y);
                maxZ = MaxFloat(maxZ, z);
                ++finiteCorners;
            }

            if (finiteCorners < 4) {
                return false;
            }

            centerX = (minX + maxX) * 0.5f;
            centerY = (minY + maxY) * 0.5f;
            centerZ = (minZ + maxZ) * 0.5f;
            const float extentX = maxX - minX;
            const float extentY = maxY - minY;
            const float extentZ = maxZ - minZ;
            radius = Length3(extentX, extentY, extentZ) * 0.5f;
            return IsFiniteFloat(centerX) && IsFiniteFloat(centerY) && IsFiniteFloat(centerZ) &&
                   IsFiniteFloat(radius) && radius > 1.0e-4f;
        }

        bool ShouldRejectCameraRelativeProxy(
            const PrimitiveDraw* draw,
            UINT vertexCount,
            const VertexBounds& bounds,
            const FixedFunctionTransformSet& transforms,
            const ProjectedBoundsMetrics& metrics,
            FixedFunctionPassReason& reason,
            char* detail,
            std::size_t detailSize)
        {
            if (!draw ||
                g_config.transformAssemblyMode != TransformAssemblyMode::SplitWorldViewProjection ||
                !transforms.valid ||
                !transforms.setWorld ||
                !transforms.setView ||
                !metrics.valid) {
                return false;
            }

            const bool largeTopology = vertexCount >= 128 || draw->primitiveCount >= 64;
            const bool largeProjection = IsNearFullscreenProjection(metrics);
            if (!largeTopology && !largeProjection) {
                return false;
            }

            float cameraX = 0.0f;
            float cameraY = 0.0f;
            float cameraZ = 0.0f;
            if (!EstimateCameraPositionFromView(transforms.view, cameraX, cameraY, cameraZ)) {
                return false;
            }

            float centerX = 0.0f;
            float centerY = 0.0f;
            float centerZ = 0.0f;
            float radius = 0.0f;
            if (!EvaluateWorldBounds(transforms.world, bounds, centerX, centerY, centerZ, radius)) {
                return false;
            }

            const float dx = centerX - cameraX;
            const float dy = centerY - cameraY;
            const float dz = centerZ - cameraZ;
            const float distance = Length3(dx, dy, dz);
            const bool largeWorld = radius >= 25.0f;
            const bool centerTracksCamera = largeWorld && distance <= MaxFloat(2.0f, radius * 0.08f);
            const bool cameraRelativeProjection =
                largeWorld && largeProjection && distance <= MaxFloat(4.0f, radius * 0.20f);
            if ((largeTopology || largeWorld) && (centerTracksCamera || cameraRelativeProjection)) {
                ++g_counters.cameraRelativeRejected;
                reason = FixedFunctionPassReason::SkippedScreenPlane;
                snprintf(
                    detail,
                    detailSize,
                    "camera-relative/skybox proxy rejected vertices=%u primitiveCount=%u radius=%g cameraDistance=%g ndcExtent=%gx%g maxNdc=%g",
                    vertexCount,
                    draw->primitiveCount,
                    radius,
                    distance,
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc);
                return true;
            }

            return false;
        }

        bool ShouldRejectProjectedProxy(
            const PrimitiveDraw* draw,
            UINT vertexCount,
            const VertexBounds& bounds,
            const FixedFunctionTransformSet* transforms,
            const Stage0TextureInfo& textureInfo,
            bool alphaTestEnabled,
            bool worldLikeAlphaTest,
            bool trustedRendererTriplet,
            FixedFunctionPassReason& reason,
            char* detail,
            std::size_t detailSize)
        {
            if (!draw || g_config.proxyGeometryFilter != ProxyGeometryFilterMode::SolidWorldOnly) {
                return false;
            }

            D3DMATRIX wvp{};
            if (!BuildWvpFromTransformSet(transforms, wvp)) {
                return false;
            }

            ProjectedBoundsMetrics metrics{};
            char metricsReason[192]{};
            if (!EvaluateProjectedBoundsMetrics(wvp, bounds, metrics, metricsReason, sizeof(metricsReason))) {
                reason = FixedFunctionPassReason::SkippedProjectedOversize;
                snprintf(detail, detailSize, "projected proxy rejected: %s", metricsReason[0] ? metricsReason : "invalid projection");
                LogProjectedProxyDecision("projectedOversize", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                return true;
            }

            const bool nearFullscreen = IsNearFullscreenProjection(metrics);
            if (transforms &&
                ShouldRejectCameraRelativeProxy(draw, vertexCount, bounds, *transforms, metrics, reason, detail, detailSize)) {
                LogProjectedProxyDecision("cameraRelative", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                return true;
            }

            const bool tinyStrip =
                static_cast<D3DPRIMITIVETYPE>(draw->primitiveType) == D3DPT_TRIANGLESTRIP &&
                vertexCount <= 4 &&
                draw->primitiveCount <= 2;
            const bool lowVertexPlane = vertexCount <= 8 && draw->primitiveCount <= 4;
            const bool screenTexturePlane = textureInfo.screenSized && nearFullscreen && (lowVertexPlane || vertexCount <= 32);
            const bool untexturedScreenStrip = tinyStrip && nearFullscreen && metrics.maxAbsNdc <= 8.0f;
            if (screenTexturePlane || untexturedScreenStrip) {
                reason = FixedFunctionPassReason::SkippedScreenPlane;
                snprintf(
                    detail,
                    detailSize,
                    "projected screen plane rejected tinyStrip=%d lowVertex=%d screenTexture=%d extent=%gx%g maxNdc=%g",
                    tinyStrip ? 1 : 0,
                    lowVertexPlane ? 1 : 0,
                    textureInfo.screenSized ? 1 : 0,
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc);
                LogProjectedProxyDecision("screenPlane", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                return true;
            }

            if (alphaTestEnabled &&
                !worldLikeAlphaTest &&
                !trustedRendererTriplet &&
                (metrics.maxAbsNdc > 4.0f || metrics.extentX > 3.5f || metrics.extentY > 3.5f)) {
                reason = FixedFunctionPassReason::SkippedProjectedOversize;
                snprintf(
                    detail,
                    detailSize,
                    "alpha-tested projected proxy rejected extent=%gx%g maxNdc=%g",
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc);
                LogProjectedProxyDecision("projectedOversize", draw, vertexCount, textureInfo, metrics, true, detail);
                return true;
            }

            const bool relaxedWorldCandidate =
                (!alphaTestEnabled || worldLikeAlphaTest) &&
                !textureInfo.screenSized &&
                !textureInfo.renderTarget &&
                transforms &&
                transforms->valid &&
                transforms->setWorld;
            const bool exceedsStrictProjection =
                metrics.maxAbsNdc > 8.0f ||
                metrics.extentX > 8.0f ||
                metrics.extentY > 8.0f;
            if (!trustedRendererTriplet && !relaxedWorldCandidate && exceedsStrictProjection) {
                reason = FixedFunctionPassReason::SkippedProjectedOversize;
                snprintf(
                    detail,
                    detailSize,
                    "strict projected proxy rejected extent=%gx%g maxNdc=%g alphaTest=%d screenTexture=%d",
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc,
                    alphaTestEnabled ? 1 : 0,
                    textureInfo.screenSized ? 1 : 0);
                LogProjectedProxyDecision("projectedOversize", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                return true;
            }

            if (!trustedRendererTriplet && relaxedWorldCandidate &&
                (metrics.maxAbsNdc > 64.0f || metrics.extentX > 64.0f || metrics.extentY > 64.0f)) {
                reason = FixedFunctionPassReason::SkippedProjectedOversize;
                snprintf(
                    detail,
                    detailSize,
                    "world-like projected proxy hard-oversize extent=%gx%g maxNdc=%g alphaTest=%d",
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc,
                    alphaTestEnabled ? 1 : 0);
                LogProjectedProxyDecision("projectedOversize", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                return true;
            }

            if (trustedRendererTriplet && exceedsStrictProjection &&
                (g_config.debugLog || g_counters.rendererTripletSubmitted < 32)) {
                snprintf(
                    detail,
                    detailSize,
                    "trusted renderer-triplet proxy accepted after camera/screen checks extent=%gx%g maxNdc=%g alphaTest=%d",
                    metrics.extentX,
                    metrics.extentY,
                    metrics.maxAbsNdc,
                    alphaTestEnabled ? 1 : 0);
                LogProjectedProxyDecision("projectedRelaxed", draw, vertexCount, textureInfo, metrics, alphaTestEnabled, detail);
                detail[0] = '\0';
            }

            if (alphaTestEnabled && (g_config.debugLog || g_counters.proxyAlphaTestSubmitted < 32)) {
                LogProjectedProxyDecision("alphaTestAccepted", draw, vertexCount, textureInfo, metrics, true, metricsReason);
            }

            return false;
        }

        bool ReadDeclarationFloats(
            const std::uint8_t* vertex,
            int offset,
            int type,
            int componentCount,
            float* values,
            int valueCapacity)
        {
            if (!vertex || !values || offset < 0 || componentCount <= 0 || componentCount > valueCapacity ||
                type < D3DDECLTYPE_FLOAT1 || type > D3DDECLTYPE_FLOAT4) {
                return false;
            }

            const float* source = reinterpret_cast<const float*>(vertex + offset);
            for (int i = 0; i < componentCount; ++i) {
                if (!IsFiniteFloat(source[i])) {
                    return false;
                }
                values[i] = source[i];
            }
            return true;
        }

        bool DecodeSkinningVertex(
            const std::uint8_t* vertex,
            const RepackLayout& layout,
            float indices[4],
            float weights[4],
            int& influenceCount)
        {
            influenceCount = 0;
            if (!ReadDeclarationFloats(
                    vertex,
                    layout.blendIndexOffset,
                    layout.blendIndexType,
                    layout.blendIndexComponents,
                    indices,
                    4) ||
                !ReadDeclarationFloats(
                    vertex,
                    layout.blendWeightOffset,
                    layout.blendWeightType,
                    layout.blendWeightComponents,
                    weights,
                    4)) {
                return false;
            }

            const int availableWeights = layout.blendWeightComponents == 1 ? 2 : layout.blendWeightComponents;
            influenceCount = layout.blendIndexComponents < availableWeights ? layout.blendIndexComponents : availableWeights;
            if (influenceCount < 1 || influenceCount > 4) {
                return false;
            }

            if (layout.blendWeightComponents == 1 && influenceCount >= 2) {
                weights[0] = ClampFloat(weights[0], 0.0f, 1.0f);
                weights[1] = 1.0f - weights[0];
            }

            float weightSum = 0.0f;
            for (int i = 0; i < influenceCount; ++i) {
                if (indices[i] < -0.01f || indices[i] > 255.01f ||
                    AbsFloat(indices[i] - std::floor(indices[i] + 0.5f)) > 0.05f ||
                    weights[i] < -0.01f || weights[i] > 1.01f) {
                    return false;
                }
                indices[i] = std::floor(indices[i] + 0.5f);
                weights[i] = ClampFloat(weights[i], 0.0f, 1.0f);
                weightSum += weights[i];
            }

            if (weightSum < 0.90f || weightSum > 1.10f) {
                return false;
            }
            const float inverseWeightSum = 1.0f / weightSum;
            for (int i = 0; i < influenceCount; ++i) {
                weights[i] *= inverseWeightSum;
            }
            return true;
        }

        bool SkinWithPalette(
            const ShaderConstantsSnapshot& constants,
            const SkinningPaletteSelection& palette,
            const float indices[4],
            const float weights[4],
            int influenceCount,
            float x,
            float y,
            float z,
            float& outX,
            float& outY,
            float& outZ)
        {
            outX = outY = outZ = 0.0f;
            for (int influence = 0; influence < influenceCount; ++influence) {
                if (weights[influence] <= 1.0e-5f) {
                    continue;
                }
                const int index = static_cast<int>(indices[influence]);
                if (palette.encoding == SkinningPaletteEncoding::QuaternionTranslation) {
                    const float* quaternion = ShaderRegister(constants, palette.baseRegister + index);
                    const float* translation = ShaderRegister(constants, palette.secondaryBaseRegister + index);
                    if (!quaternion || !translation) {
                        return false;
                    }
                    float qx = quaternion[0];
                    float qy = quaternion[1];
                    float qz = quaternion[2];
                    float qw = quaternion[3];
                    const float qLength = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
                    if (!IsFiniteFloat(qLength) || qLength < 0.5f || qLength > 1.5f) {
                        return false;
                    }
                    const float invLength = 1.0f / qLength;
                    qx *= invLength;
                    qy *= invLength;
                    qz *= invLength;
                    qw *= invLength;
                    const float tx = 2.0f * (qy * z - qz * y);
                    const float ty = 2.0f * (qz * x - qx * z);
                    const float tz = 2.0f * (qx * y - qy * x);
                    const float px = x + qw * tx + (qy * tz - qz * ty) + translation[0];
                    const float py = y + qw * ty + (qz * tx - qx * tz) + translation[1];
                    const float pz = z + qw * tz + (qx * ty - qy * tx) + translation[2];
                    if (!IsFiniteFloat(px) || !IsFiniteFloat(py) || !IsFiniteFloat(pz)) {
                        return false;
                    }
                    outX += weights[influence] * px;
                    outY += weights[influence] * py;
                    outZ += weights[influence] * pz;
                    continue;
                }
                const int firstRegister = palette.baseRegister + (palette.indicesAreRegisters ? index : index * 3);
                if (firstRegister < 0 || firstRegister + 2 >= static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT)) {
                    return false;
                }
                const float* row0 = ShaderRegister(constants, firstRegister + 0);
                const float* row1 = ShaderRegister(constants, firstRegister + 1);
                const float* row2 = ShaderRegister(constants, firstRegister + 2);
                if (!row0 || !row1 || !row2) {
                    return false;
                }
                const float px = x * row0[0] + y * row0[1] + z * row0[2] + row0[3];
                const float py = x * row1[0] + y * row1[1] + z * row1[2] + row1[3];
                const float pz = x * row2[0] + y * row2[1] + z * row2[2] + row2[3];
                if (!IsFiniteFloat(px) || !IsFiniteFloat(py) || !IsFiniteFloat(pz)) {
                    return false;
                }
                outX += weights[influence] * px;
                outY += weights[influence] * py;
                outZ += weights[influence] * pz;
            }
            return IsFiniteFloat(outX) && IsFiniteFloat(outY) && IsFiniteFloat(outZ);
        }

        bool SelectSkinningPalette(
            const std::uint8_t* source,
            UINT vertexCount,
            std::uint16_t stride,
            const RepackLayout& layout,
            const ShaderConstantsSnapshot* constants,
            SkinningPaletteSelection& selection)
        {
            if (!source || !constants || !constants->valid || vertexCount == 0 ||
                layout.blendIndexOffset < 0 || layout.blendWeightOffset < 0) {
                snprintf(selection.reason, sizeof(selection.reason), "palette inputs unavailable indices=%d weights=%d constants=%d",
                    layout.blendIndexOffset, layout.blendWeightOffset, constants && constants->valid ? 1 : 0);
                return false;
            }

            constexpr UINT sampleLimit = 96;
            SkinningPaletteSelection best{};
            SkinningPaletteSelection second{};
            for (int registerIndexMode = 0; registerIndexMode < 2; ++registerIndexMode) {
                for (int base = 0; base < static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT); ++base) {
                    float score = 0.0f;
                    UINT validSamples = 0;
                    VertexBounds sourceBounds{};
                    VertexBounds skinnedBounds{};
                    int candidateInfluenceCount = 0;
                    bool failed = false;
                    const UINT step = std::max<UINT>(1, vertexCount / sampleLimit);
                    for (UINT vertexIndex = 0; vertexIndex < vertexCount && validSamples < sampleLimit; vertexIndex += step) {
                        const std::uint8_t* vertex = source + vertexIndex * stride;
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        float indices[4]{};
                        float weights[4]{};
                        int influenceCount = 0;
                        if (!ReadFloat3(vertex, layout.positionOffset, x, y, z) ||
                            !DecodeSkinningVertex(vertex, layout, indices, weights, influenceCount)) {
                            failed = true;
                            break;
                        }
                        SkinningPaletteSelection candidate{};
                        candidate.baseRegister = base;
                        candidate.indicesAreRegisters = registerIndexMode != 0;
                        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                        if (!SkinWithPalette(*constants, candidate, indices, weights, influenceCount, x, y, z, sx, sy, sz) ||
                            MaxFloat(AbsFloat(sx), MaxFloat(AbsFloat(sy), AbsFloat(sz))) > 1.0e6f) {
                            failed = true;
                            break;
                        }
                        ExpandBounds(sourceBounds, x, y, z);
                        ExpandBounds(skinnedBounds, sx, sy, sz);
                        candidateInfluenceCount = candidateInfluenceCount > influenceCount ? candidateInfluenceCount : influenceCount;
                        ++validSamples;
                    }

                    if (failed || validSamples < 3 || !sourceBounds.valid || !skinnedBounds.valid) {
                        continue;
                    }
                    const float sourceExtent = MaxFloat(sourceBounds.maxX - sourceBounds.minX,
                        MaxFloat(sourceBounds.maxY - sourceBounds.minY, sourceBounds.maxZ - sourceBounds.minZ));
                    const float skinnedExtent = MaxFloat(skinnedBounds.maxX - skinnedBounds.minX,
                        MaxFloat(skinnedBounds.maxY - skinnedBounds.minY, skinnedBounds.maxZ - skinnedBounds.minZ));
                    if (sourceExtent < 1.0e-5f || skinnedExtent < sourceExtent * 0.05f || skinnedExtent > sourceExtent * 20.0f) {
                        continue;
                    }
                    const float ratio = skinnedExtent / sourceExtent;
                    score = 100.0f - AbsFloat(std::log(MaxFloat(ratio, 1.0e-5f))) * 12.0f + static_cast<float>(validSamples) * 0.05f;
                    SkinningPaletteSelection candidate{};
                    candidate.ok = true;
                    candidate.baseRegister = base;
                    candidate.indicesAreRegisters = registerIndexMode != 0;
                    candidate.influenceCount = candidateInfluenceCount;
                    candidate.score = score;
                    if (!best.ok || candidate.score > best.score) {
                        second = best;
                        best = candidate;
                    } else if (!second.ok || candidate.score > second.score) {
                        second = candidate;
                    }
                }
            }

            // The game uploads two 16-register arrays for skinned shaders. Probe the
            // common quaternion + translation encoding as a higher-confidence layout.
            for (int base = 0; base + 31 < static_cast<int>(SHADER_CONSTANT_REGISTER_COUNT); ++base) {
                for (int swapped = 0; swapped < 2; ++swapped) {
                    SkinningPaletteSelection candidate{};
                    candidate.encoding = SkinningPaletteEncoding::QuaternionTranslation;
                    candidate.baseRegister = swapped ? base + 16 : base;
                    candidate.secondaryBaseRegister = swapped ? base : base + 16;
                    UINT validSamples = 0;
                    float quaternionQuality = 0.0f;
                    VertexBounds sourceBounds{};
                    VertexBounds skinnedBounds{};
                    int candidateInfluenceCount = 0;
                    bool failed = false;
                    const UINT step = std::max<UINT>(1, vertexCount / sampleLimit);
                    for (UINT vertexIndex = 0; vertexIndex < vertexCount && validSamples < sampleLimit; vertexIndex += step) {
                        const std::uint8_t* vertex = source + vertexIndex * stride;
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        float indices[4]{};
                        float weights[4]{};
                        int influenceCount = 0;
                        if (!ReadFloat3(vertex, layout.positionOffset, x, y, z) ||
                            !DecodeSkinningVertex(vertex, layout, indices, weights, influenceCount)) {
                            failed = true;
                            break;
                        }
                        for (int influence = 0; influence < influenceCount; ++influence) {
                            const int bone = static_cast<int>(indices[influence]);
                            if (bone < 0 || bone >= 16) {
                                failed = true;
                                break;
                            }
                            const float* q = ShaderRegister(*constants, candidate.baseRegister + bone);
                            if (!q) {
                                failed = true;
                                break;
                            }
                            const float qLength = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                            if (!IsFiniteFloat(qLength) || qLength < 0.5f || qLength > 1.5f) {
                                failed = true;
                                break;
                            }
                            quaternionQuality += 1.0f - AbsFloat(1.0f - qLength);
                        }
                        if (failed) {
                            break;
                        }
                        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                        if (!SkinWithPalette(*constants, candidate, indices, weights, influenceCount, x, y, z, sx, sy, sz) ||
                            MaxFloat(AbsFloat(sx), MaxFloat(AbsFloat(sy), AbsFloat(sz))) > 1.0e6f) {
                            failed = true;
                            break;
                        }
                        ExpandBounds(sourceBounds, x, y, z);
                        ExpandBounds(skinnedBounds, sx, sy, sz);
                        candidateInfluenceCount = candidateInfluenceCount > influenceCount ? candidateInfluenceCount : influenceCount;
                        ++validSamples;
                    }
                    if (failed || validSamples < 3 || !sourceBounds.valid || !skinnedBounds.valid) {
                        continue;
                    }
                    const float sourceExtent = MaxFloat(sourceBounds.maxX - sourceBounds.minX,
                        MaxFloat(sourceBounds.maxY - sourceBounds.minY, sourceBounds.maxZ - sourceBounds.minZ));
                    const float skinnedExtent = MaxFloat(skinnedBounds.maxX - skinnedBounds.minX,
                        MaxFloat(skinnedBounds.maxY - skinnedBounds.minY, skinnedBounds.maxZ - skinnedBounds.minZ));
                    if (sourceExtent < 1.0e-5f || skinnedExtent < sourceExtent * 0.05f || skinnedExtent > sourceExtent * 20.0f) {
                        continue;
                    }
                    const float ratio = skinnedExtent / sourceExtent;
                    candidate.ok = true;
                    candidate.influenceCount = candidateInfluenceCount;
                    candidate.score = 125.0f - AbsFloat(std::log(MaxFloat(ratio, 1.0e-5f))) * 12.0f +
                        quaternionQuality / static_cast<float>(validSamples * candidateInfluenceCount + 1) * 8.0f;
                    if (!best.ok || candidate.score > best.score) {
                        second = best;
                        best = candidate;
                    } else if (!second.ok || candidate.score > second.score) {
                        second = candidate;
                    }
                }
            }

            if (!best.ok || (second.ok && best.score - second.score < 0.25f)) {
                snprintf(selection.reason, sizeof(selection.reason), "no unique palette best=%g second=%g",
                    best.ok ? best.score : 0.0f, second.ok ? second.score : 0.0f);
                return false;
            }
            selection = best;
            snprintf(selection.reason, sizeof(selection.reason), "palette encoding=%s base=%d secondary=%d indexMode=%s influences=%d score=%g",
                selection.encoding == SkinningPaletteEncoding::QuaternionTranslation ? "quatTranslation" : "matrix3x4",
                selection.baseRegister,
                selection.secondaryBaseRegister,
                selection.indicesAreRegisters ? "register" : "bone",
                selection.influenceCount,
                selection.score);
            return true;
        }

        bool RepackVertices(
            IDirect3DDevice9* device,
            const PrimitiveDraw* draw,
            IDirect3DVertexBuffer9* sourceBuffer,
            UINT sourceOffset,
            UINT firstVertex,
            UINT vertexCount,
            std::uint16_t stride,
            const RepackLayout& layout,
            bool forceWhiteDiffuse,
            VertexBounds* bounds,
            const ShaderConstantsSnapshot* shaderConstants,
            SkinningPaletteSelection* paletteSelection)
        {
            if (!sourceBuffer || !EnsureScratchVertexBuffer(device, vertexCount)) {
                return false;
            }

            const UINT lockOffset = sourceOffset + firstVertex * stride;
            const UINT lockSize = vertexCount * stride;
            void* sourceData = nullptr;
            HRESULT hr = sourceBuffer->Lock(lockOffset, lockSize, &sourceData, D3DLOCK_READONLY);
            if (FAILED(hr)) {
                hr = sourceBuffer->Lock(lockOffset, lockSize, &sourceData, 0);
            }
            if (FAILED(hr) || !sourceData) {
                LogAlways("repack source Lock failed first=%u count=%u stride=%u hr=0x%08lx", firstVertex, vertexCount, stride, static_cast<unsigned long>(hr));
                return false;
            }

            void* scratchData = nullptr;
            hr = g_scratchVertexBuffer->Lock(0, vertexCount * sizeof(ScratchVertex), &scratchData, D3DLOCK_DISCARD);
            if (FAILED(hr) || !scratchData) {
                sourceBuffer->Unlock();
                LogAlways("repack scratch Lock failed count=%u hr=0x%08lx", vertexCount, static_cast<unsigned long>(hr));
                return false;
            }

            const auto* source = reinterpret_cast<const std::uint8_t*>(sourceData);
            auto* destination = reinterpret_cast<ScratchVertex*>(scratchData);
            const bool skinned = layout.hasBlendIndices || layout.hasBlendWeight;
            SkinningPaletteSelection palette{};
            if (skinned && !SelectSkinningPalette(source, vertexCount, stride, layout, shaderConstants, palette)) {
                if (paletteSelection) {
                    *paletteSelection = palette;
                }
                g_scratchVertexBuffer->Unlock();
                sourceBuffer->Unlock();
                return false;
            }
            if (paletteSelection) {
                *paletteSelection = palette;
            }
            for (UINT i = 0; i < vertexCount; ++i) {
                const std::uint8_t* vertex = source + i * stride;
                ScratchVertex& out = destination[i];
                float skinIndices[4]{};
                float skinWeights[4]{};
                int skinInfluenceCount = 0;
                if (skinned && !DecodeSkinningVertex(vertex, layout, skinIndices, skinWeights, skinInfluenceCount)) {
                    g_scratchVertexBuffer->Unlock();
                    sourceBuffer->Unlock();
                    return false;
                }
                if (!ReadFloat3(vertex, layout.positionOffset, out.x, out.y, out.z)) {
                    out.x = out.y = out.z = 0.0f;
                } else if (skinned) {
                    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                    if (!SkinWithPalette(*shaderConstants, palette, skinIndices, skinWeights, skinInfluenceCount, out.x, out.y, out.z, sx, sy, sz)) {
                        g_scratchVertexBuffer->Unlock();
                        sourceBuffer->Unlock();
                        return false;
                    }
                    out.x = sx;
                    out.y = sy;
                    out.z = sz;
                }
                if (bounds && IsFiniteFloat(out.x) && IsFiniteFloat(out.y) && IsFiniteFloat(out.z)) {
                    ExpandBounds(*bounds, out.x, out.y, out.z);
                }

                if (layout.hasNormal && layout.normalType == D3DDECLTYPE_FLOAT3 &&
                    ReadNormalizedFloat3(vertex, layout.normalOffset, out.nx, out.ny, out.nz)) {
                    // Copied below through the fixed-function normal FVF.
                } else if (layout.hasNormal && layout.normalType == D3DDECLTYPE_D3DCOLOR) {
                    const DWORD packed = *reinterpret_cast<const DWORD*>(vertex + layout.normalOffset);
                    out.nx = (static_cast<float>((packed >> 16) & 0xFFu) / 127.5f) - 1.0f;
                    out.ny = (static_cast<float>((packed >> 8) & 0xFFu) / 127.5f) - 1.0f;
                    out.nz = (static_cast<float>(packed & 0xFFu) / 127.5f) - 1.0f;
                    Normalize3(out.nx, out.ny, out.nz);
                } else {
                    out.nx = 0.0f;
                    out.ny = 0.0f;
                    out.nz = 0.0f;
                }
                if (skinned && (out.nx != 0.0f || out.ny != 0.0f || out.nz != 0.0f)) {
                    float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
                    float normalX = 0.0f, normalY = 0.0f, normalZ = 0.0f;
                    if (!SkinWithPalette(*shaderConstants, palette, skinIndices, skinWeights, skinInfluenceCount,
                            0.0f, 0.0f, 0.0f, originX, originY, originZ) ||
                        !SkinWithPalette(*shaderConstants, palette, skinIndices, skinWeights, skinInfluenceCount,
                            out.nx, out.ny, out.nz, normalX, normalY, normalZ)) {
                        g_scratchVertexBuffer->Unlock();
                        sourceBuffer->Unlock();
                        return false;
                    }
                    out.nx = normalX - originX;
                    out.ny = normalY - originY;
                    out.nz = normalZ - originZ;
                    Normalize3(out.nx, out.ny, out.nz);
                }

                out.diffuse = (!forceWhiteDiffuse && layout.hasDiffuse)
                    ? *reinterpret_cast<const DWORD*>(vertex + layout.diffuseOffset)
                    : 0xFFFFFFFFu;

                if (layout.hasTexcoord) {
                    const float* uv = reinterpret_cast<const float*>(vertex + layout.texcoordOffset);
                    out.u = IsFiniteFloat(uv[0]) ? uv[0] : 0.0f;
                    out.v = IsFiniteFloat(uv[1]) ? uv[1] : 0.0f;
                } else {
                    out.u = 0.0f;
                    out.v = 0.0f;
                }
            }

            if (layout.hasNormal) {
                ++g_counters.repackNormalsCopied;
            } else {
                const UINT generated =
                    draw && draw->indexBuffer
                        ? GenerateIndexedFlatNormals(device, destination, vertexCount, draw)
                        : GenerateUnindexedFlatNormals(destination, vertexCount, draw);
                if (generated != 0) {
                    ++g_counters.repackNormalsGenerated;
                } else {
                    ++g_counters.repackNormalsDefaulted;
                    for (UINT i = 0; i < vertexCount; ++i) {
                        destination[i].nx = 0.0f;
                        destination[i].ny = 1.0f;
                        destination[i].nz = 0.0f;
                    }
                }
            }

            g_scratchVertexBuffer->Unlock();
            sourceBuffer->Unlock();
            return true;
        }

        class ScopedStreamSourceOverride
        {
        public:
            ScopedStreamSourceOverride(IDirect3DDevice9* device, IDirect3DVertexBuffer9* replacement, UINT stride)
                : device_(device)
            {
                if (!device_ || !replacement) {
                    return;
                }

                if (FAILED(device_->GetStreamSource(0, &savedStream_, &savedOffset_, &savedStride_))) {
                    savedStream_ = nullptr;
                    savedOffset_ = 0;
                    savedStride_ = 0;
                }

                active_ = SUCCEEDED(device_->SetStreamSource(0, replacement, 0, stride));
            }

            ~ScopedStreamSourceOverride()
            {
                if (device_ && active_) {
                    device_->SetStreamSource(0, savedStream_, savedOffset_, savedStride_);
                }

                if (savedStream_) {
                    savedStream_->Release();
                    savedStream_ = nullptr;
                }
            }

            bool Active() const { return active_; }

        private:
            IDirect3DDevice9* device_ = nullptr;
            IDirect3DVertexBuffer9* savedStream_ = nullptr;
            UINT savedOffset_ = 0;
            UINT savedStride_ = 0;
            bool active_ = false;
        };

        class ScopedFixedFunctionState
        {
        public:
            ScopedFixedFunctionState(
                IDirect3DDevice9* device,
                DWORD fvf,
                const FixedFunctionTransformSet* transforms,
                const ShaderConstantsSnapshot* shaderConstants,
                const RatatouilleShaderLayout* shaderLayout,
                bool useTexture,
                bool proxyOnly,
                bool visibleProxy,
                bool proxyColorWrites,
                bool preserveAlphaTest)
                : device_(device)
            {
                if (!device_) {
                    return;
                }

                device_->GetVertexDeclaration(&savedVertexDeclaration_);
                device_->GetVertexShader(&savedVertexShader_);
                device_->GetPixelShader(&savedPixelShader_);
                device_->GetFVF(&savedFvf_);

                SaveTss(0, D3DTSS_COLOROP);
                SaveTss(0, D3DTSS_COLORARG1);
                SaveTss(0, D3DTSS_COLORARG2);
                SaveTss(0, D3DTSS_ALPHAOP);
                SaveTss(0, D3DTSS_ALPHAARG1);
                SaveTss(0, D3DTSS_ALPHAARG2);
                SaveTss(0, D3DTSS_TEXTURETRANSFORMFLAGS);
                SaveTss(1, D3DTSS_COLOROP);
                SaveTss(1, D3DTSS_ALPHAOP);
                SaveRs(D3DRS_LIGHTING);
                SaveRs(D3DRS_COLORVERTEX);
                SaveRs(D3DRS_AMBIENT);
                SaveRs(D3DRS_COLORWRITEENABLE);
                SaveRs(D3DRS_ZWRITEENABLE);
                SaveRs(D3DRS_ZFUNC);
                SaveRs(D3DRS_ALPHABLENDENABLE);
                SaveRs(D3DRS_SRCBLEND);
                SaveRs(D3DRS_DESTBLEND);
                SaveRs(D3DRS_ALPHATESTENABLE);
                SaveRs(D3DRS_ALPHAFUNC);
                SaveRs(D3DRS_ALPHAREF);
                SaveRs(D3DRS_FOGENABLE);
                SaveRs(D3DRS_TEXTUREFACTOR);
                SaveTransform(0, D3DTS_WORLD);
                SaveTransform(1, D3DTS_VIEW);
                SaveTransform(2, D3DTS_PROJECTION);
                savedMaterialValid_ = SUCCEEDED(device_->GetMaterial(&savedMaterial_));
                for (UINT i = 0; i < savedLights_.size(); ++i) {
                    savedLightValid_[i] = SUCCEEDED(device_->GetLight(i, &savedLights_[i]));
                    savedLightEnableValid_[i] = SUCCEEDED(device_->GetLightEnable(i, &savedLightEnabled_[i]));
                }

                device_->SetVertexShader(nullptr);
                device_->SetPixelShader(nullptr);
                device_->SetFVF(fvf);
                ApplyTransforms(transforms);
                const bool fullbright = UsesDiagnosticFixedFunctionMaterial(proxyOnly, visibleProxy);
                const bool textureless =
                    fullbright &&
                    g_config.fixedFunctionMaterialMode == FixedFunctionMaterialMode::TexturelessWhite;

                if (fullbright) {
                    D3DMATERIAL9 material{};
                    material.Diffuse.r = 1.0f;
                    material.Diffuse.g = 1.0f;
                    material.Diffuse.b = 1.0f;
                    material.Diffuse.a = 1.0f;
                    material.Ambient = material.Diffuse;
                    material.Emissive.a = 1.0f;
                    material.Specular.a = 1.0f;
                    device_->SetMaterial(&material);
                    device_->SetRenderState(D3DRS_AMBIENT, 0xFFFFFFFFu);
                    device_->SetRenderState(D3DRS_FOGENABLE, FALSE);
                    device_->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
                    device_->SetRenderState(
                        D3DRS_COLORWRITEENABLE,
                        D3DCOLORWRITEENABLE_RED |
                            D3DCOLORWRITEENABLE_GREEN |
                            D3DCOLORWRITEENABLE_BLUE |
                            D3DCOLORWRITEENABLE_ALPHA);
                    device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    if (!preserveAlphaTest) {
                        device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                    }
                }
                bool lightingEnabled = false;
                ApplyRatatouilleMaterial(shaderConstants, shaderLayout);
                if (g_config.lightBridgeMode == LightBridgeMode::DebugDirectional) {
                    ApplyDebugLight();
                    lightingEnabled = true;
                } else if (g_config.lightBridgeMode == LightBridgeMode::ShaderConstants) {
                    lightingEnabled = ApplyRatatouilleLights(shaderConstants, shaderLayout);
                }

                device_->SetRenderState(D3DRS_LIGHTING, lightingEnabled ? TRUE : FALSE);
                device_->SetRenderState(D3DRS_COLORVERTEX, TRUE);
                if (proxyOnly) {
                    if (proxyColorWrites) {
                        device_->SetRenderState(
                            D3DRS_COLORWRITEENABLE,
                            D3DCOLORWRITEENABLE_RED |
                                D3DCOLORWRITEENABLE_GREEN |
                                D3DCOLORWRITEENABLE_BLUE |
                                D3DCOLORWRITEENABLE_ALPHA);
                    } else {
                        device_->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
                    }
                    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                    if (visibleProxy) {
                        device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    } else if (proxyColorWrites) {
                        device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                        device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                        device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
                        device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
                    }
                } else if (fullbright) {
                    device_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                    device_->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                }
                if (useTexture && !textureless) {
                    device_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    device_->SetTextureStageState(0, D3DTSS_COLORARG2, fullbright ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
                    device_->SetTextureStageState(0, D3DTSS_COLOROP, fullbright ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
                    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                    device_->SetTextureStageState(0, D3DTSS_ALPHAARG2, fullbright ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
                    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, fullbright ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
                } else {
                    device_->SetTextureStageState(0, D3DTSS_COLORARG1, fullbright ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
                    device_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, fullbright ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
                    device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                }
                device_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                device_->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                device_->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                active_ = true;
            }

            ~ScopedFixedFunctionState()
            {
                Restore();
            }

            ScopedFixedFunctionState(const ScopedFixedFunctionState&) = delete;
            ScopedFixedFunctionState& operator=(const ScopedFixedFunctionState&) = delete;

        private:
            struct SavedTss
            {
                DWORD stage;
                D3DTEXTURESTAGESTATETYPE state;
                DWORD value;
            };

            struct SavedRs
            {
                D3DRENDERSTATETYPE state;
                DWORD value;
            };

            struct SavedTransform
            {
                D3DTRANSFORMSTATETYPE state;
                D3DMATRIX value;
                bool valid;
            };

            void SaveTss(DWORD stage, D3DTEXTURESTAGESTATETYPE state)
            {
                DWORD value = 0;
                if (SUCCEEDED(device_->GetTextureStageState(stage, state, &value)) && tssCount_ < savedTss_.size()) {
                    savedTss_[tssCount_++] = { stage, state, value };
                }
            }

            void SaveRs(D3DRENDERSTATETYPE state)
            {
                DWORD value = 0;
                if (SUCCEEDED(device_->GetRenderState(state, &value)) && rsCount_ < savedRs_.size()) {
                    savedRs_[rsCount_++] = { state, value };
                }
            }

            void SaveTransform(std::size_t slot, D3DTRANSFORMSTATETYPE state)
            {
                if (slot >= savedTransforms_.size()) {
                    return;
                }

                savedTransforms_[slot].state = state;
                savedTransforms_[slot].valid = SUCCEEDED(device_->GetTransform(state, &savedTransforms_[slot].value));
            }

            void RefreshTransform(std::size_t slot)
            {
                if (slot < savedTransforms_.size() && savedTransforms_[slot].valid) {
                    device_->SetTransform(savedTransforms_[slot].state, &savedTransforms_[slot].value);
                }
            }

            void ApplyTransforms(const FixedFunctionTransformSet* transforms)
            {
                if (!transforms || !transforms->valid) {
                    RefreshTransform(0);
                    RefreshTransform(1);
                    RefreshTransform(2);
                    return;
                }

                if (transforms->setWorld) {
                    device_->SetTransform(D3DTS_WORLD, &transforms->world);
                } else {
                    RefreshTransform(0);
                }

                if (transforms->setView) {
                    device_->SetTransform(D3DTS_VIEW, &transforms->view);
                } else {
                    RefreshTransform(1);
                }

                if (transforms->setProjection) {
                    device_->SetTransform(D3DTS_PROJECTION, &transforms->projection);
                } else {
                    RefreshTransform(2);
                }
            }

            D3DCOLORVALUE MakeColorValue(const float* rgba, float alphaFallback, float maxValue = 16.0f)
            {
                D3DCOLORVALUE color{};
                color.r = rgba ? ClampFloat(rgba[0], 0.0f, maxValue) : 1.0f;
                color.g = rgba ? ClampFloat(rgba[1], 0.0f, maxValue) : 1.0f;
                color.b = rgba ? ClampFloat(rgba[2], 0.0f, maxValue) : 1.0f;
                color.a = rgba && IsFiniteFloat(rgba[3]) ? ClampFloat(rgba[3], 0.0f, 1.0f) : alphaFallback;
                return color;
            }

            D3DCOLORVALUE MakeBlackColorValue(float alpha = 1.0f)
            {
                D3DCOLORVALUE color{};
                color.a = alpha;
                return color;
            }

            D3DCOLORVALUE MakeScaledColorValue(const float* rgba, float alphaFallback, float maxValue, float scale)
            {
                D3DCOLORVALUE color{};
                color.r = rgba ? ClampFloat(rgba[0] * scale, 0.0f, maxValue) : 0.0f;
                color.g = rgba ? ClampFloat(rgba[1] * scale, 0.0f, maxValue) : 0.0f;
                color.b = rgba ? ClampFloat(rgba[2] * scale, 0.0f, maxValue) : 0.0f;
                color.a = rgba && IsFiniteFloat(rgba[3]) ? ClampFloat(rgba[3], 0.0f, 1.0f) : alphaFallback;
                return color;
            }

            float ColorEnergy(const float* rgba, float scale = 1.0f)
            {
                if (!rgba || !IsFiniteFloat(scale)) {
                    return 0.0f;
                }

                return MaxFloat(
                    MaxFloat(AbsFloat(rgba[0] * scale), AbsFloat(rgba[1] * scale)),
                    AbsFloat(rgba[2] * scale));
            }

            void ApplyRatatouilleMaterial(const ShaderConstantsSnapshot* shaderConstants, const RatatouilleShaderLayout* shaderLayout)
            {
                if (!shaderConstants || !shaderConstants->valid || !shaderLayout || !HasShaderRegisters(*shaderLayout)) {
                    return;
                }

                const float* diffuse = ShaderRegister(*shaderConstants, shaderLayout->materialRegister + 2);
                const float* emissive = ShaderRegister(*shaderConstants, shaderLayout->materialRegister + 3);
                const float* specular = ShaderRegister(*shaderConstants, shaderLayout->materialRegister + 4);
                if (!diffuse && !emissive && !specular) {
                    return;
                }

                D3DMATERIAL9 material{};
                material.Diffuse = MakeColorValue(diffuse, 1.0f, 1.0f);
                material.Ambient = material.Diffuse;
                material.Emissive = MakeBlackColorValue();
                const float emissiveScale = ClampFloat(g_config.shaderMaterialEmissiveScale, 0.0f, 1.0f);
                if (emissive && emissiveScale > 0.0f) {
                    material.Emissive = MakeScaledColorValue(
                        emissive,
                        1.0f,
                        RTX_SHADER_MATERIAL_EMISSIVE_MAX,
                        emissiveScale);
                }
                material.Specular = specular ? MakeColorValue(specular, 1.0f, 1.0f) : MakeBlackColorValue();
                material.Power = specular && IsFiniteFloat(specular[3]) ? ClampFloat(specular[3], 0.0f, 256.0f) : 0.0f;
                device_->SetMaterial(&material);
                ++g_counters.shaderMaterialDraws;
            }

            bool ApplyRatatouilleLights(const ShaderConstantsSnapshot* shaderConstants, const RatatouilleShaderLayout* shaderLayout)
            {
                if (!shaderConstants || !shaderConstants->valid || !shaderLayout || !HasShaderRegisters(*shaderLayout)) {
                    return false;
                }

                auto colorTripletFinite = [](const float* rgba) -> bool {
                    return rgba && IsFiniteFloat(rgba[0]) && IsFiniteFloat(rgba[1]) && IsFiniteFloat(rgba[2]);
                };

                std::array<D3DLIGHT9, RTX_CAPTURE_LIGHT_COUNT> decodedLights{};
                UINT decoded = 0;
                const float* dlightDir = ShaderRegister(*shaderConstants, shaderLayout->lightRegister + 1);
                const float* dlightAmbient = ShaderRegister(*shaderConstants, shaderLayout->lightRegister + 2);
                const float* dlightColor = ShaderRegister(*shaderConstants, shaderLayout->lightRegister + 3);
                DWORD ambientDword = 0;
                if (dlightAmbient) {
                    ambientDword = ColorToDword(
                        ClampFloat(dlightAmbient[0], 0.0f, RTX_SHADER_LIGHT_AMBIENT_MAX),
                        ClampFloat(dlightAmbient[1], 0.0f, RTX_SHADER_LIGHT_AMBIENT_MAX),
                        ClampFloat(dlightAmbient[2], 0.0f, RTX_SHADER_LIGHT_AMBIENT_MAX),
                        IsFiniteFloat(dlightAmbient[3]) ? ClampFloat(dlightAmbient[3], 0.0f, 1.0f) : 1.0f);
                }

                float dirX = dlightDir ? -dlightDir[0] : 0.0f;
                float dirY = dlightDir ? -dlightDir[1] : -1.0f;
                float dirZ = dlightDir ? -dlightDir[2] : 0.0f;
                if (decoded < decodedLights.size() &&
                    colorTripletFinite(dlightColor) &&
                    Normalize3(dirX, dirY, dirZ) &&
                    ColorEnergy(dlightColor) > 1.0e-4f) {
                    D3DLIGHT9 light{};
                    light.Type = D3DLIGHT_DIRECTIONAL;
                    light.Diffuse = MakeColorValue(dlightColor, 1.0f, RTX_SHADER_LIGHT_COLOR_MAX);
                    light.Specular = light.Diffuse;
                    light.Ambient = MakeBlackColorValue();
                    light.Direction.x = dirX;
                    light.Direction.y = dirY;
                    light.Direction.z = dirZ;
                    decodedLights[decoded++] = light;
                }

                for (int omni = 0; omni < shaderLayout->omniCount && decoded < RTX_CAPTURE_LIGHT_COUNT; ++omni) {
                    const int base = shaderLayout->omniRegister + omni * shaderLayout->omniSize;
                    const float* position = ShaderRegister(*shaderConstants, base + 0);
                    const float* direction = ShaderRegister(*shaderConstants, base + 1);
                    const float* color = ShaderRegister(*shaderConstants, base + 2);
                    const float* attenuation = ShaderRegister(*shaderConstants, base + 3);
                    if (!position || !direction || !colorTripletFinite(color) || !attenuation) {
                        continue;
                    }

                    const float diffuseScale = IsFiniteFloat(color[3]) ? ClampFloat(color[3], 0.0f, 1.0f) : 1.0f;
                    if (ColorEnergy(color, diffuseScale) <= 1.0e-4f) {
                        continue;
                    }

                    D3DLIGHT9 light{};
                    light.Position.x = position[0];
                    light.Position.y = position[1];
                    light.Position.z = position[2];
                    if (!IsFiniteFloat(light.Position.x) || !IsFiniteFloat(light.Position.y) || !IsFiniteFloat(light.Position.z)) {
                        continue;
                    }

                    const float startSq = IsFiniteFloat(position[3]) ? MaxFloat(position[3], 0.0f) : 0.0f;
                    const float att = IsFiniteFloat(attenuation[0]) ? MaxFloat(attenuation[0], 0.0f) : 0.0f;
                    light.Range = att > 1.0e-8f
                        ? ClampFloat(std::sqrt(startSq + (1.0f / att)), 1.0f, RTX_SHADER_LIGHT_RANGE_MAX)
                        : RTX_SHADER_LIGHT_RANGE_MAX;
                    light.Attenuation0 = 0.0f;
                    light.Attenuation1 = 0.0f;
                    light.Attenuation2 = att > 1.0e-8f ? att : 1.0f / (light.Range * light.Range);
                    light.Diffuse = MakeColorValue(color, 1.0f, RTX_SHADER_LIGHT_COLOR_MAX);
                    light.Diffuse.r *= diffuseScale;
                    light.Diffuse.g *= diffuseScale;
                    light.Diffuse.b *= diffuseScale;
                    light.Specular = light.Diffuse;
                    light.Ambient.r = 0.0f;
                    light.Ambient.g = 0.0f;
                    light.Ambient.b = 0.0f;
                    light.Ambient.a = 1.0f;

                    float spotX = direction[0];
                    float spotY = direction[1];
                    float spotZ = direction[2];
                    const bool hasSpotDirection = Normalize3(spotX, spotY, spotZ);
                    const bool looksSpot =
                        hasSpotDirection &&
                        ((IsFiniteFloat(attenuation[1]) && AbsFloat(attenuation[1]) > 1.0e-4f) ||
                         (IsFiniteFloat(attenuation[2]) && AbsFloat(attenuation[2]) > 1.0e-4f));
                    if (looksSpot) {
                        light.Type = D3DLIGHT_SPOT;
                        light.Direction.x = spotX;
                        light.Direction.y = spotY;
                        light.Direction.z = spotZ;
                        light.Theta = 0.65f;
                        light.Phi = 1.20f;
                        light.Falloff = 1.0f;
                    } else {
                        light.Type = D3DLIGHT_POINT;
                    }

                    decodedLights[decoded++] = light;
                }

                if (decoded == 0) {
                    return false;
                }

                std::uint32_t hash = 2166136261u;
                HashValue(hash, ambientDword);
                HashValue(hash, decoded);
                for (UINT i = 0; i < decoded; ++i) {
                    const D3DLIGHT9& light = decodedLights[i];
                    HashValue(hash, static_cast<int>(light.Type));
                    HashValue(hash, light.Diffuse.r);
                    HashValue(hash, light.Diffuse.g);
                    HashValue(hash, light.Diffuse.b);
                    HashValue(hash, light.Position.x);
                    HashValue(hash, light.Position.y);
                    HashValue(hash, light.Position.z);
                    HashValue(hash, light.Direction.x);
                    HashValue(hash, light.Direction.y);
                    HashValue(hash, light.Direction.z);
                    HashValue(hash, light.Range);
                    HashValue(hash, light.Attenuation2);
                    HashValue(hash, light.Theta);
                    HashValue(hash, light.Phi);
                }

                g_counters.shaderLightsSeen += decoded;
                const bool changed = !g_shaderLightHashValid || g_shaderLightHash != hash;
                if (changed) {
                    ++g_counters.shaderLightSetsChanged;
                    g_shaderLightHash = hash;
                    g_shaderLightHashValid = true;
                } else {
                    ++g_counters.shaderLightSetsReused;
                }

                if (g_shaderLightSetsAppliedThisFrame >= RTX_SHADER_LIGHT_SETS_PER_FRAME) {
                    g_counters.shaderLightsSuppressed += decoded;
                    return false;
                }

                for (UINT i = 0; i < RTX_CAPTURE_LIGHT_COUNT; ++i) {
                    device_->LightEnable(i, FALSE);
                }
                device_->SetRenderState(D3DRS_AMBIENT, ambientDword);

                UINT submitted = 0;
                UINT directional = 0;
                UINT point = 0;
                UINT spot = 0;
                for (UINT i = 0; i < decoded; ++i) {
                    if (SUCCEEDED(device_->SetLight(i, &decodedLights[i]))) {
                        device_->LightEnable(i, TRUE);
                        ++submitted;
                        if (decodedLights[i].Type == D3DLIGHT_DIRECTIONAL) {
                            ++directional;
                        } else if (decodedLights[i].Type == D3DLIGHT_SPOT) {
                            ++spot;
                        } else {
                            ++point;
                        }
                    }
                }

                if (submitted == 0) {
                    return false;
                }

                ++g_shaderLightSetsAppliedThisFrame;
                ++g_counters.shaderLightSetsApplied;
                g_counters.shaderLegacyLightsSubmitted += submitted;
                g_counters.shaderDirectionalLights += directional;
                g_counters.shaderPointLights += point;
                g_counters.shaderSpotLights += spot;
                if ((g_config.logFile || g_config.debugLog) && g_counters.shaderLightSetsApplied <= 24) {
                    LogAlways(
                        "shader lights applied layout=%s set=%llu count=%u hash=0x%08x changed=%d ambient=0x%08lx",
                        shaderLayout->name,
                        static_cast<unsigned long long>(g_counters.shaderLightSetsApplied),
                        submitted,
                        hash,
                        changed ? 1 : 0,
                        static_cast<unsigned long>(ambientDword));
                }
                return true;
            }

            void ApplyDebugLight()
            {
                if (g_config.lightBridgeMode != LightBridgeMode::DebugDirectional) {
                    return;
                }

                D3DMATERIAL9 material{};
                material.Diffuse.r = 1.0f;
                material.Diffuse.g = 1.0f;
                material.Diffuse.b = 1.0f;
                material.Diffuse.a = 1.0f;
                material.Ambient = material.Diffuse;
                device_->SetMaterial(&material);

                D3DLIGHT9 light{};
                light.Type = D3DLIGHT_DIRECTIONAL;
                light.Diffuse.r = 1.0f;
                light.Diffuse.g = 0.96f;
                light.Diffuse.b = 0.88f;
                light.Diffuse.a = 1.0f;
                light.Specular = light.Diffuse;
                light.Ambient.r = 0.18f;
                light.Ambient.g = 0.18f;
                light.Ambient.b = 0.20f;
                light.Ambient.a = 1.0f;
                light.Direction.x = 0.35f;
                light.Direction.y = -0.75f;
                light.Direction.z = 0.55f;
                device_->SetLight(0, &light);
                device_->LightEnable(0, TRUE);
                device_->SetRenderState(D3DRS_AMBIENT, 0x30303030);
                ++g_counters.debugLightDraws;
            }

            void Restore()
            {
                if (!active_ || !device_) {
                    return;
                }

                for (std::size_t i = 0; i < rsCount_; ++i) {
                    device_->SetRenderState(savedRs_[i].state, savedRs_[i].value);
                }

                for (std::size_t i = tssCount_; i > 0; --i) {
                    const SavedTss& saved = savedTss_[i - 1];
                    device_->SetTextureStageState(saved.stage, saved.state, saved.value);
                }

                for (const SavedTransform& saved : savedTransforms_) {
                    if (saved.valid) {
                        device_->SetTransform(saved.state, &saved.value);
                    }
                }

                if (savedMaterialValid_) {
                    device_->SetMaterial(&savedMaterial_);
                }
                for (UINT i = 0; i < savedLights_.size(); ++i) {
                    if (savedLightValid_[i]) {
                        device_->SetLight(i, &savedLights_[i]);
                    }
                    if (savedLightEnableValid_[i]) {
                        device_->LightEnable(i, savedLightEnabled_[i]);
                    }
                }

                if (savedVertexDeclaration_) {
                    device_->SetVertexDeclaration(savedVertexDeclaration_);
                } else {
                    device_->SetFVF(savedFvf_);
                }

                device_->SetVertexShader(savedVertexShader_);
                device_->SetPixelShader(savedPixelShader_);

                if (savedVertexDeclaration_) {
                    savedVertexDeclaration_->Release();
                    savedVertexDeclaration_ = nullptr;
                }
                if (savedVertexShader_) {
                    savedVertexShader_->Release();
                    savedVertexShader_ = nullptr;
                }
                if (savedPixelShader_) {
                    savedPixelShader_->Release();
                    savedPixelShader_ = nullptr;
                }

                active_ = false;
            }

            IDirect3DDevice9* device_ = nullptr;
            IDirect3DVertexDeclaration9* savedVertexDeclaration_ = nullptr;
            IDirect3DVertexShader9* savedVertexShader_ = nullptr;
            IDirect3DPixelShader9* savedPixelShader_ = nullptr;
            DWORD savedFvf_ = 0;
            std::array<SavedTss, 9> savedTss_{};
            std::array<SavedRs, 16> savedRs_{};
            std::array<SavedTransform, 3> savedTransforms_{};
            D3DMATERIAL9 savedMaterial_{};
            std::array<D3DLIGHT9, RTX_CAPTURE_LIGHT_COUNT> savedLights_{};
            std::array<BOOL, RTX_CAPTURE_LIGHT_COUNT> savedLightEnabled_{};
            std::array<bool, RTX_CAPTURE_LIGHT_COUNT> savedLightValid_{};
            std::array<bool, RTX_CAPTURE_LIGHT_COUNT> savedLightEnableValid_{};
            std::size_t tssCount_ = 0;
            std::size_t rsCount_ = 0;
            bool savedMaterialValid_ = false;
            bool active_ = false;
        };

        HRESULT DrawOriginalPass(IDirect3DDevice9* device, const PrimitiveDraw* draw)
        {
            if (draw->indexBuffer) {
                return device->DrawIndexedPrimitive(
                    static_cast<D3DPRIMITIVETYPE>(draw->primitiveType),
                    0,
                    draw->minVertexIndex,
                    draw->numVertices,
                    draw->startIndex,
                    draw->primitiveCount);
            }

            return device->DrawPrimitive(
                static_cast<D3DPRIMITIVETYPE>(draw->primitiveType),
                draw->startVertex,
                draw->primitiveCount);
        }

        FixedFunctionPassResult DrawRepackedPass(
            IDirect3DDevice9* device,
            const D3DRendererZ* renderer,
            const PrimitiveDraw* draw,
            const RepackLayout& layout,
            const MatrixSelection* preselectedTransform,
            const FixedFunctionTransformSet* preselectedTransformSet,
            bool preselectedTransformIsWorld,
            const ShaderConstantsSnapshot* shaderConstants,
            const RatatouilleShaderLayout* shaderLayout,
            bool hasTexture,
            const Stage0TextureInfo& textureInfo,
            bool alphaTestEnabled,
            bool proxyOnly,
            bool visibleProxy,
            FixedFunctionPassResult result)
        {
            IDirect3DVertexBuffer9* sourceStream = nullptr;
            UINT sourceOffset = 0;
            UINT sourceStride = 0;
            HRESULT hr = device->GetStreamSource(0, &sourceStream, &sourceOffset, &sourceStride);
            if (FAILED(hr) || !sourceStream) {
                LogAlways("repack GetStreamSource failed hr=0x%08lx", static_cast<unsigned long>(hr));
                if (sourceStream) {
                    sourceStream->Release();
                }
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedRepack;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = FAILED(hr) ? hr : D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::RepackSourceUnavailable;
                SetPassDetail(result, "GetStreamSource failed hr=0x%08lx", static_cast<unsigned long>(hr));
                return result;
            }

            if (sourceStride != draw->stride) {
                DebugLog("repack stream stride mismatch current=%u draw=%u", sourceStride, draw->stride);
            }

            UINT firstVertex = 0;
            UINT vertexCount = 0;
            if (draw->indexBuffer) {
                firstVertex = 0;
                vertexCount = static_cast<UINT>(draw->minVertexIndex) + static_cast<UINT>(draw->numVertices);
            } else {
                firstVertex = draw->startVertex;
                vertexCount = PrimitiveVertexCount(draw->primitiveType, draw->primitiveCount);
            }

            if (vertexCount == 0 || vertexCount > g_config.repackMaxVertices) {
                if (sourceStream) {
                    sourceStream->Release();
                }
                ++g_counters.repackFallbacks;
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedRepack;
                }
                LogAlways("repack vertex count rejected count=%u max=%u indexed=%d", vertexCount, g_config.repackMaxVertices, draw->indexBuffer ? 1 : 0);
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::VertexCountRejected;
                SetPassDetail(result, "vertex count rejected count=%u max=%u indexed=%d", vertexCount, g_config.repackMaxVertices, draw->indexBuffer ? 1 : 0);
                return result;
            }

            char indexReason[160]{};
            if (!ValidateIndexedDrawRange(device, draw, vertexCount, indexReason, sizeof(indexReason))) {
                sourceStream->Release();
                ++g_counters.invalidIndexRanges;
                if (g_config.debugLog || g_counters.invalidIndexRanges <= 16) {
                    LogAlways(
                        "fixed-function reject invalid index range stride=%u vertices=%u primitive=%u reason=\"%s\"",
                        draw->stride,
                        vertexCount,
                        draw->primitiveType,
                        indexReason);
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::InvalidIndexRange;
                CopyText(result.detail, sizeof(result.detail), indexReason);
                return result;
            }

            VertexBounds bounds{};
            const bool forceWhiteDiffuse = UsesDiagnosticFixedFunctionMaterial(proxyOnly, visibleProxy);
            SkinningPaletteSelection paletteSelection{};
            const bool skinnedLayout = layout.hasBlendIndices || layout.hasBlendWeight;
            const bool repacked = RepackVertices(
                device,
                draw,
                sourceStream,
                sourceOffset,
                firstVertex,
                vertexCount,
                draw->stride,
                layout,
                forceWhiteDiffuse,
                &bounds,
                shaderConstants,
                &paletteSelection);
            sourceStream->Release();
            if (!repacked) {
                if (proxyOnly && skinnedLayout) {
                    ++g_counters.skinnedPaletteMissing;
                    ++g_counters.skinnedPaletteIncomplete;
                    CountProxySkipReason(FixedFunctionPassReason::SkippedSkinned);
                    SetSkippedProxyResult(
                        result,
                        FixedFunctionPassReason::SkippedSkinned,
                        paletteSelection.reason[0] ? paletteSelection.reason : "skinning palette unavailable");
                    return result;
                }
                ++g_counters.repackFallbacks;
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedRepack;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::RepackLockFailed;
                SetPassDetail(result, "RepackVertices failed first=%u count=%u stride=%u", firstVertex, vertexCount, draw->stride);
                return result;
            }
            if (proxyOnly && skinnedLayout) {
                ++g_counters.skinnedPaletteComplete;
            }

            char boundsReason[160]{};
            const bool needsBounds =
                proxyOnly ||
                IsRuntimeDiagnosticSubmission(g_config.submissionMode) ||
                g_config.transformMode == RtxTransformMode::ShaderConstantsWvp;
            if (needsBounds && !ValidateVertexBounds(bounds, boundsReason, sizeof(boundsReason))) {
                if (proxyOnly) {
                    ++g_counters.proxySkippedBadBounds;
                }
                if (g_config.debugLog || g_counters.proxySkippedBadBounds <= 16) {
                    LogAlways(
                        "proxy skip bad bounds stride=%u vertices=%u primitive=%u reason=\"%s\"",
                        draw->stride,
                        vertexCount,
                        draw->primitiveType,
                        boundsReason);
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::BadVertexBounds;
                CopyText(result.detail, sizeof(result.detail), boundsReason);
                return result;
            }

            MatrixSelection drawTransformSelection{};
            FixedFunctionTransformSet transformSet{};
            const FixedFunctionTransformSet* transformSetPtr = nullptr;
            bool transformIsWorld = false;
            const RatatouilleShaderLayout* drawShaderLayout = shaderLayout;
            if (preselectedTransform) {
                drawTransformSelection = *preselectedTransform;
                transformIsWorld = preselectedTransformIsWorld;
                if (g_config.transformMode != RtxTransformMode::DeviceTransforms) {
                    transformSet = preselectedTransformSet && preselectedTransformSet->valid
                        ? *preselectedTransformSet
                        : BuildLegacyTransformSet(&drawTransformSelection.matrix, transformIsWorld);
                    transformSetPtr = &transformSet;
                }
                if (proxyOnly &&
                    transformIsWorld &&
                    transformSetPtr &&
                    !(transformSet.setView && transformSet.setProjection) &&
                    g_config.allowUnsafeShaderWvp &&
                    shaderConstants &&
                    shaderConstants->valid &&
                    IsAfterOriginalProxySubmission(g_config.submissionMode)) {
                    RatatouilleTransformSelection cameraSelection{};
                    if (SelectRatatouilleShaderCameraForWorld(
                            device,
                            *shaderConstants,
                            drawTransformSelection.matrix,
                            bounds,
                            cameraSelection)) {
                        transformSet = cameraSelection.transforms;
                        transformSetPtr = &transformSet;
                        drawShaderLayout = cameraSelection.layout;
                        drawTransformSelection.transposed = cameraSelection.transposed;
                        drawTransformSelection.score = cameraSelection.score;
                        drawTransformSelection.startRegister = cameraSelection.layout ? cameraSelection.layout->viewRegister : -1;
                        CopyText(drawTransformSelection.source, sizeof(drawTransformSelection.source), "rendererSlot5World+shaderCam");
                        CopyText(drawTransformSelection.reason, sizeof(drawTransformSelection.reason), cameraSelection.reason);
                        CopyText(result.shaderLayout, sizeof(result.shaderLayout), cameraSelection.layout ? cameraSelection.layout->name : "(none)");
                    } else if (cameraSelection.screenRejected) {
                        ++g_counters.screenMatrixSkips;
                        ++g_counters.proxySkippedScreenMatrix;
                        result.status = FixedFunctionPassStatus::SkippedUi;
                        result.hr = S_OK;
                        result.reason = FixedFunctionPassReason::SkippedScreenMatrix;
                        CopyText(result.transformSource, sizeof(result.transformSource), "rendererSlot5World+shaderCam");
                        CopyText(result.transformReason, sizeof(result.transformReason), cameraSelection.reason);
                        SetPassDetail(result, "%s", cameraSelection.reason[0] ? cameraSelection.reason : "shader camera rejected as screen-space");
                        return result;
                    } else if (cameraSelection.reason[0]) {
                        CopyText(drawTransformSelection.reason, sizeof(drawTransformSelection.reason), cameraSelection.reason);
                    }
                }
            } else if (g_config.transformMode == RtxTransformMode::RatatouilleShaderConstants) {
                RatatouilleTransformSelection ratSelection{};
                if (!shaderConstants || !SelectRatatouilleShaderTransforms(device, *shaderConstants, bounds, ratSelection)) {
                    if (proxyOnly) {
                        if (ratSelection.affineRejected) {
                            ++g_counters.proxySkippedBadAffine;
                        } else {
                            ++g_counters.proxySkippedNoTransform;
                        }
                    }
                    if (ratSelection.screenRejected) {
                        ++g_counters.screenMatrixSkips;
                        if (proxyOnly) {
                            ++g_counters.proxySkippedScreenMatrix;
                        }
                        result.status = FixedFunctionPassStatus::SkippedUi;
                        result.hr = S_OK;
                        result.reason = FixedFunctionPassReason::SkippedScreenMatrix;
                        SetPassDetail(result, "%s", ratSelection.reason[0] ? ratSelection.reason : "screen-space transform rejected");
                        return result;
                    }
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = ratSelection.affineRejected ? FixedFunctionPassReason::BadAffine : FixedFunctionPassReason::NoTransform;
                    CopyText(result.transformSource, sizeof(result.transformSource), "ratatouilleShaderConstants");
                    CopyText(result.transformReason, sizeof(result.transformReason), ratSelection.reason);
                    SetPassDetail(result, "%s", ratSelection.reason[0] ? ratSelection.reason : "Ratatouille shader constants unavailable");
                    return result;
                }

                transformSet = ratSelection.transforms;
                transformSetPtr = &transformSet;
                transformIsWorld = true;
                drawShaderLayout = ratSelection.layout;
                drawTransformSelection.ok = true;
                drawTransformSelection.matrix = transformSet.world;
                drawTransformSelection.transposed = ratSelection.transposed;
                drawTransformSelection.score = ratSelection.score;
                drawTransformSelection.startRegister = ratSelection.layout ? ratSelection.layout->worldRegister : -1;
                CopyText(drawTransformSelection.source, sizeof(drawTransformSelection.source), "ratatouilleShaderConstants");
                CopyText(drawTransformSelection.reason, sizeof(drawTransformSelection.reason), ratSelection.reason);
                CopyText(result.shaderLayout, sizeof(result.shaderLayout), ratSelection.layout ? ratSelection.layout->name : "(none)");
            } else if (g_config.transformMode == RtxTransformMode::ShaderConstantsWvp) {
                if (SelectShaderConstantWvpMatrix(device, bounds, drawTransformSelection)) {
                    LogMatrixSelection(drawTransformSelection);
                    LogDrawWvpSelection("live shader", draw, bounds, drawTransformSelection);
                    transformSet = BuildLegacyTransformSet(&drawTransformSelection.matrix, false);
                    transformSetPtr = &transformSet;
                    transformIsWorld = false;
                } else {
                    MatrixSelection shaderSelection = drawTransformSelection;
                    ++g_counters.wvpNoCandidate;
                    if (g_config.debugLog || g_counters.wvpNoCandidate <= 16) {
                        LogAlways(
                            "wvpNoCandidate stride=%u vertices=%u primitive=%u bounds=\"%s\" reason=\"%s\"",
                            draw->stride,
                            vertexCount,
                            draw->primitiveType,
                            boundsReason[0] ? boundsReason : "not checked",
                            drawTransformSelection.reason[0] ? drawTransformSelection.reason : "unknown");
                    }

                    if (proxyOnly) {
                        ++g_counters.proxySkippedNoTransform;
                    }
                    if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction) {
                        ++g_counters.badRuntimeTransforms;
                    }
                    LogCommandConstantWvpProbe(device, bounds, shaderSelection.reason);
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = FixedFunctionPassReason::NoWvpCandidate;
                    CopyText(result.transformSource, sizeof(result.transformSource), "shaderConstantsWvp");
                    CopyText(result.transformReason, sizeof(result.transformReason), shaderSelection.reason);
                    SetPassDetail(
                        result,
                        "shaderConstantsWvp failed: %s; matrix probes are informational only",
                        shaderSelection.reason[0] ? shaderSelection.reason : "unknown");
                    return result;
                }
            } else {
                if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction) {
                    ++g_counters.badRuntimeTransforms;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::NoTransform;
                SetPassDetail(result, "no transform selection supplied for repack mode=%s", TransformModeName(g_config.transformMode));
                return result;
            }

            result.transformOk = true;
            result.transformTransposed = drawTransformSelection.transposed;
            result.transformIsWorld = transformIsWorld;
            CopyText(result.transformSource, sizeof(result.transformSource), drawTransformSelection.source);
            CopyText(result.transformReason, sizeof(result.transformReason), drawTransformSelection.reason);
            if (drawShaderLayout && !result.shaderLayout[0]) {
                CopyText(result.shaderLayout, sizeof(result.shaderLayout), drawShaderLayout->name);
            }
            if (proxyOnly &&
                transformSetPtr &&
                result.proxyClass != ProxyDrawClass::Ui &&
                g_config.transformMode != RtxTransformMode::RendererMatrixCache) {
                char cameraReason[192]{};
                if (!ValidateOrLockFrameCamera(device, *transformSetPtr, cameraReason, sizeof(cameraReason))) {
                    CountProxySkipReason(FixedFunctionPassReason::SkippedCameraMismatch);
                    SetSkippedProxyResult(result, FixedFunctionPassReason::SkippedCameraMismatch, cameraReason);
                    return result;
                }
            }

            if (proxyOnly && result.proxyClass != ProxyDrawClass::Ui) {
                FixedFunctionPassReason projectionReason = FixedFunctionPassReason::None;
                char projectionDetail[192]{};
                const bool worldLikeAlphaTest =
                    alphaTestEnabled &&
                    g_config.proxyAlphaTestMode == ProxyAlphaTestMode::SolidWorldMasked &&
                    result.proxyClass == ProxyDrawClass::World &&
                    result.renderStateSnapshotValid &&
                    result.alphaBlendEnable == FALSE &&
                    result.zWriteEnable != FALSE &&
                    result.colorWriteMask != 0 &&
                    !textureInfo.screenSized &&
                    !textureInfo.renderTarget;
                const bool trustedRendererTriplet =
                    g_config.transformMode == RtxTransformMode::RendererMatrixCache &&
                    transformSetPtr &&
                    transformSetPtr->valid &&
                    transformSetPtr->setWorld &&
                    transformSetPtr->setView &&
                    transformSetPtr->setProjection;
                if (ShouldRejectProjectedProxy(
                        draw,
                        vertexCount,
                        bounds,
                        transformSetPtr,
                        textureInfo,
                        alphaTestEnabled,
                        worldLikeAlphaTest,
                        trustedRendererTriplet,
                        projectionReason,
                        projectionDetail,
                        sizeof(projectionDetail))) {
                    if (alphaTestEnabled) {
                        ++g_counters.proxyAlphaTestRejected;
                    }
                    CountProxySkipReason(projectionReason);
                    SetSkippedProxyResult(result, projectionReason, projectionDetail);
                    return result;
                }
            }

            if (transformIsWorld && !(transformSet.setView && transformSet.setProjection)) {
                char projectedReason[192]{};
                if (!ValidateWorldProjectedBounds(device, drawTransformSelection.matrix, bounds, projectedReason, sizeof(projectedReason))) {
                    LogCommandConstantWvpProbe(device, bounds, projectedReason);
                    if (proxyOnly) {
                        ++g_counters.proxySkippedBadViewProj;
                    }
                    if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction) {
                        ++g_counters.badRuntimeTransforms;
                    }
                    if (g_config.debugLog || g_counters.proxySkippedBadViewProj <= 16) {
                        LogAlways("proxy skip bad view/projection reason=\"%s\"", projectedReason);
                    }
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = FixedFunctionPassReason::BadViewProjection;
                    CopyText(result.detail, sizeof(result.detail), projectedReason);
                    return result;
                }

                CopyText(result.transformReason, sizeof(result.transformReason), projectedReason);
            }

            ScopedFixedFunctionState state(
                device,
                SCRATCH_VERTEX_FVF,
                transformSetPtr,
                shaderConstants,
                drawShaderLayout,
                hasTexture,
                proxyOnly,
                visibleProxy,
                result.colorWritesEnabled,
                result.alphaTestEnabled && ShouldPreserveFixedFunctionAlphaTest(proxyOnly));
            ScopedStreamSourceOverride streamOverride(device, g_scratchVertexBuffer, sizeof(ScratchVertex));
            if (!streamOverride.Active()) {
                ++g_counters.repackFallbacks;
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedRepack;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::StreamOverrideFailed;
                SetPassDetail(result, "SetStreamSource scratch override failed vertices=%u", vertexCount);
                return result;
            }

            if (draw->indexBuffer) {
                hr = device->DrawIndexedPrimitive(
                    static_cast<D3DPRIMITIVETYPE>(draw->primitiveType),
                    0,
                    draw->minVertexIndex,
                    draw->numVertices,
                    draw->startIndex,
                    draw->primitiveCount);
            } else {
                hr = device->DrawPrimitive(
                    static_cast<D3DPRIMITIVETYPE>(draw->primitiveType),
                    0,
                    draw->primitiveCount);
            }

            result.status = SUCCEEDED(hr) ? FixedFunctionPassStatus::ConvertedRepacked : FixedFunctionPassStatus::Unsupported;
            result.hr = hr;
            result.reason = SUCCEEDED(hr) ? FixedFunctionPassReason::ConvertedRepacked : FixedFunctionPassReason::DrawFailed;
            if (SUCCEEDED(hr) && proxyOnly && g_config.transformMode == RtxTransformMode::RendererMatrixCache) {
                ++g_counters.rendererTripletSubmitted;
            }
            if (SUCCEEDED(hr) && proxyOnly && alphaTestEnabled) {
                ++g_counters.proxyAlphaTestSubmitted;
                if (result.proxyClass == ProxyDrawClass::World &&
                    g_config.proxyAlphaTestMode == ProxyAlphaTestMode::SolidWorldMasked) {
                    ++g_counters.maskedWorldSubmitted;
                }
            }
            if (SUCCEEDED(hr) && proxyOnly && skinnedLayout) {
                ++g_counters.skinnedSubmitted;
                SetPassDetail(
                    result,
                    "skinned repack vertices=%u encoding=%s paletteBase=%d secondary=%d indexMode=%s influences=%d score=%g hr=0x%08lx",
                    vertexCount,
                    paletteSelection.encoding == SkinningPaletteEncoding::QuaternionTranslation ? "quatTranslation" : "matrix3x4",
                    paletteSelection.baseRegister,
                    paletteSelection.secondaryBaseRegister,
                    paletteSelection.indicesAreRegisters ? "register" : "bone",
                    paletteSelection.influenceCount,
                    paletteSelection.score,
                    static_cast<unsigned long>(hr));
                return result;
            }
            if (SUCCEEDED(hr) && proxyOnly && result.proxyClass == ProxyDrawClass::Ui) {
                ++g_counters.uiProxySubmitted;
                ++g_counters.uiOrthographicSubmitted;
            } else if (SUCCEEDED(hr) && proxyOnly && result.proxyClass == ProxyDrawClass::Sky) {
                ++g_counters.skyProxySubmitted;
                ++g_counters.skyWorldSubmitted;
            }
            if (FAILED(hr) && proxyOnly) {
                ++g_counters.proxyDrawFailures;
            }
            SetPassDetail(result, "repacked draw vertices=%u hr=0x%08lx", vertexCount, static_cast<unsigned long>(hr));
            return result;
        }

        FixedFunctionPassResult DrawFixedFunctionPass(
            IDirect3DDevice9* device,
            const D3DRendererZ* renderer,
            const PrimitiveDraw* draw,
            bool proxyOnly,
            bool visibleProxy,
            bool proxyColorWrites)
        {
            FixedFunctionPassResult result{};
            result.proxyOnly = proxyOnly;
            result.visibleProxy = visibleProxy;
            result.colorWritesEnabled = !proxyOnly || proxyColorWrites;
            result.noOpBlend = proxyOnly && proxyColorWrites && !visibleProxy;
            DWORD alphaTest = FALSE;
            result.alphaTestEnabled = SUCCEEDED(device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest)) && alphaTest != FALSE;
            CopyText(result.transformSource, sizeof(result.transformSource), TransformModeName(g_config.transformMode));
            CapturePassStateSnapshot(device, result);
            const Stage0TextureInfo textureInfo = QueryStage0TextureInfo(device);
            CopyTextureInfoToResult(result, textureInfo);
            const bool hasTexture = textureInfo.hasTexture;
            char screenCopyDetail[192]{};
            const bool screenCopyTexture = textureInfo.renderTarget ||
                Stage0TextureLooksLikeScreenCopy(device, screenCopyDetail, sizeof(screenCopyDetail));
            const bool classifiedProxyEnabled =
                proxyOnly &&
                g_config.proxyUiSkyMode == ProxyUiSkyMode::ClassifiedProxy &&
                result.colorWriteMask != 0 &&
                !screenCopyTexture;
            bool deferredStateFilter = false;
            FixedFunctionPassReason deferredFilterReason = FixedFunctionPassReason::None;
            char deferredFilterDetail[192]{};

            ShaderConstantsSnapshot shaderConstants{};
            const ShaderConstantsSnapshot* shaderConstantsPtr = nullptr;
            const bool needsShaderConstants =
                g_config.transformMode == RtxTransformMode::RatatouilleShaderConstants ||
                (proxyOnly &&
                    IsAfterOriginalProxySubmission(g_config.submissionMode) &&
                    IsWorldMatrixTransformMode(g_config.transformMode) &&
                    g_config.allowUnsafeShaderWvp) ||
                (proxyOnly && g_config.proxySkinnedMode == ProxySkinnedMode::PaletteSkinning) ||
                g_config.lightBridgeMode == LightBridgeMode::ShaderConstants;
            if (needsShaderConstants) {
                char shaderConstantsReason[160]{};
                if (ReadShaderConstants(device, shaderConstants, shaderConstantsReason, sizeof(shaderConstantsReason))) {
                    shaderConstantsPtr = &shaderConstants;
                } else if (g_config.debugLog || g_counters.matrixFailures <= 8) {
                    LogAlways("shader constant capture failed: %s", shaderConstantsReason[0] ? shaderConstantsReason : "unknown");
                }
            }
            const RatatouilleShaderLayout* shaderLayoutForState =
                shaderConstantsPtr ? &RATATOUILLE_SHADER_LAYOUTS[0] : nullptr;

            DWORD zEnable = result.zEnable;
            if (SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE, &zEnable)) && zEnable == D3DZB_FALSE) {
                result.zEnable = zEnable;
                if (!classifiedProxyEnabled) {
                    ++g_counters.zDisabledSkippedDraws;
                    if (proxyOnly) {
                        ++g_counters.proxySkippedZDisabled;
                    }
                    result.status = FixedFunctionPassStatus::SkippedUi;
                    result.hr = S_OK;
                    result.reason = FixedFunctionPassReason::SkippedZDisabled;
                    SetPassDetail(result, "D3DRS_ZENABLE is false");
                    return result;
                }
            }

            if (proxyOnly && g_config.proxyGeometryFilter == ProxyGeometryFilterMode::SolidWorldOnly) {
                FixedFunctionPassReason filterReason = FixedFunctionPassReason::None;
                char filterDetail[192]{};
                if (SolidWorldRenderStateShouldSkip(device, filterReason, filterDetail, sizeof(filterDetail))) {
                    const bool classifiedStateCandidate = classifiedProxyEnabled &&
                        (result.zEnable == D3DZB_FALSE || result.zWriteEnable == FALSE);
                    if (classifiedStateCandidate && filterReason == FixedFunctionPassReason::SkippedTransparent) {
                        deferredStateFilter = true;
                        deferredFilterReason = filterReason;
                        CopyText(deferredFilterDetail, sizeof(deferredFilterDetail), filterDetail);
                    } else {
                        if (result.alphaTestEnabled) {
                            ++g_counters.proxyAlphaTestRejected;
                        }
                        CountProxySkipReason(filterReason);
                        SetSkippedProxyResult(result, filterReason, filterDetail);
                        return result;
                    }
                }
            }

            if (IsRuntimeDiagnosticSubmission(g_config.submissionMode)) {
                DWORD alphaBlend = 0;
                DWORD zWrite = TRUE;
                if (SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) &&
                    SUCCEEDED(device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) &&
                    alphaBlend != FALSE &&
                    zWrite == FALSE) {
                    ++g_counters.uiSkippedDraws;
                    result.status = FixedFunctionPassStatus::SkippedUi;
                    result.hr = S_OK;
                    result.reason = FixedFunctionPassReason::SkippedLikelyUi;
                    SetPassDetail(result, "alpha blended draw with ZWRITE disabled is treated as UI/overlay");
                    return result;
                }
            }

            DWORD mappedFvf = 0;
            FvfBuildResult fvfResult{};
            const bool directFvfOk = BuildFvfFromDeclaration(device, draw->stride, mappedFvf, fvfResult);
            result.directFvfOk = directFvfOk;
            result.fvf = mappedFvf;
            result.mappedStride = fvfResult.mappedStride;
            CopyText(result.fvfReason, sizeof(result.fvfReason), fvfResult.reason);

            LogFvfAttempt(fvfResult, draw->stride);
            if (directFvfOk && (fvfResult.rhw || IsRhwFvf(mappedFvf))) {
                if (classifiedProxyEnabled) {
                    result.proxyClass = ProxyDrawClass::Ui;
                    ScopedFixedFunctionState state(
                        device,
                        mappedFvf,
                        nullptr,
                        shaderConstantsPtr,
                        shaderLayoutForState,
                        hasTexture,
                        proxyOnly,
                        visibleProxy,
                        result.colorWritesEnabled,
                        result.alphaTestEnabled && ShouldPreserveFixedFunctionAlphaTest(proxyOnly));
                    const HRESULT hr = DrawOriginalPass(device, draw);
                    result.status = SUCCEEDED(hr) ? FixedFunctionPassStatus::ConvertedDirect : FixedFunctionPassStatus::Unsupported;
                    result.hr = hr;
                    result.path = FixedFunctionPassPath::DirectFvf;
                    result.reason = SUCCEEDED(hr) ? FixedFunctionPassReason::ConvertedDirect : FixedFunctionPassReason::DrawFailed;
                    if (SUCCEEDED(hr)) {
                        ++g_counters.uiProxySubmitted;
                        ++g_counters.uiRhwSubmitted;
                    } else {
                        ++g_counters.proxyDrawFailures;
                    }
                    SetPassDetail(result, "classified RHW UI proxy hr=0x%08lx", static_cast<unsigned long>(hr));
                    return result;
                }
                ++g_counters.rhwSkippedDraws;
                if (proxyOnly) {
                    ++g_counters.proxySkippedRhw;
                }
                result.status = FixedFunctionPassStatus::SkippedUi;
                result.hr = S_OK;
                result.path = FixedFunctionPassPath::DirectFvf;
                result.reason = FixedFunctionPassReason::SkippedRhw;
                SetPassDetail(result, "RHW/POSITIONT declaration is screen-space");
                return result;
            }

            const bool wvpFromBounds = g_config.transformMode == RtxTransformMode::ShaderConstantsWvp;
            const bool ratatouilleFromBounds = g_config.transformMode == RtxTransformMode::RatatouilleShaderConstants;
            const bool shaderCameraForWorldProxy =
                proxyOnly &&
                IsAfterOriginalProxySubmission(g_config.submissionMode) &&
                IsWorldMatrixTransformMode(g_config.transformMode);
            const bool forceRepack =
                visibleProxy ||
                wvpFromBounds ||
                ratatouilleFromBounds ||
                shaderCameraForWorldProxy ||
                deferredStateFilter ||
                IsRuntimeDiagnosticSubmission(g_config.submissionMode);
            MatrixSelection matrixSelection{};
            FixedFunctionTransformSet selectedTransformSet{};
            const FixedFunctionTransformSet* selectedTransformSetPtr = nullptr;
            bool transformIsWorld = false;
            bool rendererProjectionOrthographic = false;
            const D3DMATRIX* transform = nullptr;
            if (!wvpFromBounds && !ratatouilleFromBounds) {
                const bool selectedRendererTriplet = g_config.transformMode == RtxTransformMode::RendererMatrixCache;
                const bool transformSelected = selectedRendererTriplet
                    ? SelectRendererMatrixCacheMatrix(
                        renderer,
                        device,
                        matrixSelection,
                        &selectedTransformSet,
                        &rendererProjectionOrthographic)
                    : SelectTransformMatrix(device, renderer, matrixSelection);
                if (!transformSelected) {
                    CopyText(result.transformSource, sizeof(result.transformSource), matrixSelection.source);
                    CopyText(result.transformReason, sizeof(result.transformReason), matrixSelection.reason);
                    if (proxyOnly) {
                        if (matrixSelection.affineRejected) {
                            ++g_counters.proxySkippedBadAffine;
                        } else {
                            ++g_counters.proxySkippedNoTransform;
                        }
                    }
                    if (matrixSelection.screenRejected) {
                        ++g_counters.screenMatrixSkips;
                        if (proxyOnly) {
                            ++g_counters.proxySkippedScreenMatrix;
                        }
                        result.status = FixedFunctionPassStatus::SkippedUi;
                        result.hr = S_OK;
                        result.reason = FixedFunctionPassReason::SkippedScreenMatrix;
                        SetPassDetail(result, "%s", matrixSelection.reason[0] ? matrixSelection.reason : "screen-space transform rejected");
                        return result;
                    }

                    ++g_counters.noWorldMatrixFallbacks;
                    if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction) {
                        ++g_counters.badRuntimeTransforms;
                    }
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = matrixSelection.affineRejected ? FixedFunctionPassReason::BadAffine : FixedFunctionPassReason::NoTransform;
                    SetPassDetail(result, "%s", matrixSelection.reason[0] ? matrixSelection.reason : "transform selection failed");
                    return result;
                }

                transformIsWorld =
                    g_config.transformMode == RtxTransformMode::RendererSlot5World ||
                    g_config.transformMode == RtxTransformMode::RendererMatrixCache;
                result.transformOk = true;
                result.transformTransposed = matrixSelection.transposed;
                result.transformIsWorld = transformIsWorld;
                CopyText(result.transformSource, sizeof(result.transformSource), matrixSelection.source);
                CopyText(result.transformReason, sizeof(result.transformReason), matrixSelection.reason);
                if (transformIsWorld && !forceRepack) {
                    char viewProjReason[160]{};
                    if (!ValidateDeviceViewProjection(device, viewProjReason, sizeof(viewProjReason))) {
                        if (proxyOnly) {
                            ++g_counters.proxySkippedBadViewProj;
                        }
                        if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction) {
                            ++g_counters.badRuntimeTransforms;
                        }
                        if (g_config.debugLog || g_counters.proxySkippedBadViewProj <= 16) {
                            LogAlways("proxy skip bad view/projection reason=\"%s\"", viewProjReason);
                        }
                        result.status = FixedFunctionPassStatus::Unsupported;
                        result.hr = D3DERR_INVALIDCALL;
                        result.reason = FixedFunctionPassReason::BadViewProjection;
                        CopyText(result.detail, sizeof(result.detail), viewProjReason);
                        return result;
                    }
                }
                transform = g_config.transformMode == RtxTransformMode::DeviceTransforms ? nullptr : &matrixSelection.matrix;
                if (selectedRendererTriplet) {
                    selectedTransformSetPtr = &selectedTransformSet;
                } else if (transform) {
                    selectedTransformSet = BuildLegacyTransformSet(transform, transformIsWorld);
                    selectedTransformSetPtr = &selectedTransformSet;
                }
            } else {
                CopyText(
                    result.transformSource,
                    sizeof(result.transformSource),
                    ratatouilleFromBounds ? "ratatouilleShaderConstants" : "shaderConstantsWvp");
            }

            if (directFvfOk && !forceRepack) {
                ScopedFixedFunctionState state(
                    device,
                    mappedFvf,
                    selectedTransformSetPtr,
                    shaderConstantsPtr,
                    shaderLayoutForState,
                    hasTexture,
                    proxyOnly,
                    visibleProxy,
                    result.colorWritesEnabled,
                    result.alphaTestEnabled && ShouldPreserveFixedFunctionAlphaTest(proxyOnly));
                const HRESULT hr = DrawOriginalPass(device, draw);
                result.status = SUCCEEDED(hr) ? FixedFunctionPassStatus::ConvertedDirect : FixedFunctionPassStatus::Unsupported;
                result.hr = hr;
                result.path = FixedFunctionPassPath::DirectFvf;
                result.reason = SUCCEEDED(hr) ? FixedFunctionPassReason::ConvertedDirect : FixedFunctionPassReason::DrawFailed;
                if (FAILED(hr) && proxyOnly) {
                    ++g_counters.proxyDrawFailures;
                }
                SetPassDetail(result, "direct FVF draw hr=0x%08lx", static_cast<unsigned long>(hr));
                return result;
            }

            if (g_config.vertexBridgeMode == VertexBridgeMode::DirectFvf && !forceRepack) {
                ++g_counters.fvfFailures;
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedFvf;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::DirectFvfOnlyFailed;
                SetPassDetail(result, "directFvf mode rejected declaration: %s", fvfResult.reason[0] ? fvfResult.reason : "unknown");
                return result;
            }

            if (!IsTrianglePrimitiveType(draw->primitiveType)) {
                ++g_counters.nonTriangleFallbacks;
                if (proxyOnly) {
                    ++g_counters.proxySkippedNonTriangle;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::NonTrianglePrimitive;
                SetPassDetail(result, "primitive type %u is not triangle geometry", draw->primitiveType);
                return result;
            }

            RepackLayout layout{};
            if (!BuildRepackLayout(device, draw->stride, layout)) {
                ++g_counters.fvfFailures;
                ++g_counters.repackFallbacks;
                LogRepackLayout(layout, draw->stride, hasTexture);
                if (proxyOnly) {
                    ++g_counters.proxyUnsupportedRepack;
                }
                result.status = FixedFunctionPassStatus::Unsupported;
                result.hr = D3DERR_INVALIDCALL;
                result.reason = FixedFunctionPassReason::RepackLayoutFailed;
                CopyText(result.repackReason, sizeof(result.repackReason), layout.reason);
                SetPassDetail(result, "repack layout rejected: %s", layout.reason[0] ? layout.reason : "unknown");
                return result;
            }

            LogRepackLayout(layout, draw->stride, hasTexture);
            result.path = FixedFunctionPassPath::Repacked;
            CopyText(result.repackReason, sizeof(result.repackReason), layout.reason);
            if (classifiedProxyEnabled) {
                if (result.zEnable == D3DZB_FALSE) {
                    if (rendererProjectionOrthographic && selectedTransformSetPtr && selectedTransformSetPtr->valid) {
                        result.proxyClass = ProxyDrawClass::Ui;
                    } else {
                        ++g_counters.uiHeuristicRejected;
                        ++g_counters.proxySkippedZDisabled;
                        CountProxySkipReason(FixedFunctionPassReason::SkippedZDisabled);
                        SetSkippedProxyResult(
                            result,
                            FixedFunctionPassReason::SkippedZDisabled,
                            "Z-disabled non-RHW draw lacks a validated orthographic renderer projection");
                        return result;
                    }
                } else if (deferredStateFilter &&
                    result.zWriteEnable == FALSE &&
                    !rendererProjectionOrthographic &&
                    selectedTransformSetPtr &&
                    selectedTransformSetPtr->valid &&
                    selectedTransformSetPtr->setWorld &&
                    selectedTransformSetPtr->setView &&
                    selectedTransformSetPtr->setProjection &&
                    !layout.hasNormalSemantic &&
                    hasTexture &&
                    !textureInfo.screenSized &&
                    !textureInfo.renderTarget) {
                    result.proxyClass = ProxyDrawClass::Sky;
                } else if (deferredStateFilter && result.zWriteEnable == FALSE) {
                    ++g_counters.skyHeuristicRejected;
                }
            }

            if (deferredStateFilter && result.proxyClass == ProxyDrawClass::World) {
                if (result.alphaTestEnabled) {
                    ++g_counters.proxyAlphaTestRejected;
                }
                CountProxySkipReason(deferredFilterReason);
                SetSkippedProxyResult(
                    result,
                    deferredFilterReason,
                    deferredFilterDetail[0] ? deferredFilterDetail : "classified proxy rejected unrecognized transparent world draw");
                return result;
            }

            if (proxyOnly) {
                FixedFunctionPassReason filterReason = FixedFunctionPassReason::None;
                char filterDetail[192]{};
                if (result.proxyClass == ProxyDrawClass::World &&
                    SolidWorldLayoutShouldSkip(layout, hasTexture, textureInfo, filterReason, filterDetail, sizeof(filterDetail))) {
                    if (filterReason == FixedFunctionPassReason::MissingTexcoord) {
                        ++g_counters.noTexcoordFallbacks;
                    }
                    if (result.alphaTestEnabled) {
                        ++g_counters.proxyAlphaTestRejected;
                    }
                    CountProxySkipReason(filterReason);
                    SetSkippedProxyResult(result, filterReason, filterDetail);
                    return result;
                }

            }

            if (layout.rhw) {
                ++g_counters.rhwSkippedDraws;
                if (proxyOnly) {
                    ++g_counters.proxySkippedRhw;
                }
                result.status = FixedFunctionPassStatus::SkippedUi;
                result.hr = S_OK;
                result.reason = FixedFunctionPassReason::SkippedRhw;
                SetPassDetail(result, "repack layout is RHW/POSITIONT");
                return result;
            }

            bool effectiveHasTexture = hasTexture;
            if (hasTexture && !layout.hasTexcoord) {
                ++g_counters.noTexcoordFallbacks;
                if (proxyOnly) {
                    ++g_counters.proxySkippedNoTexcoord;
                }
                if (g_config.debugLog || g_counters.noTexcoordFallbacks <= 16) {
                    LogAlways("repack textureless fallback: texture bound but TEXCOORD0 missing stride=%u proxy=%d", draw->stride, proxyOnly ? 1 : 0);
                }
                if (proxyOnly && !g_config.captureAllowTexturelessProxy) {
                    ++g_counters.repackFallbacks;
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = FixedFunctionPassReason::MissingTexcoord;
                    SetPassDetail(result, "texture bound but TEXCOORD0 missing; textureless proxy capture disabled");
                    return result;
                }
                effectiveHasTexture = false;
                if (!proxyOnly && !UsesDiagnosticFixedFunctionMaterial(proxyOnly, visibleProxy)) {
                    ++g_counters.repackFallbacks;
                    result.status = FixedFunctionPassStatus::Unsupported;
                    result.hr = D3DERR_INVALIDCALL;
                    result.reason = FixedFunctionPassReason::MissingTexcoord;
                    SetPassDetail(result, "texture bound but TEXCOORD0 missing");
                    return result;
                }
            }

            result.hasTexture = effectiveHasTexture;
            return DrawRepackedPass(
                device,
                renderer,
                draw,
                layout,
                (wvpFromBounds || ratatouilleFromBounds) ? nullptr : &matrixSelection,
                selectedTransformSetPtr,
                transformIsWorld,
                shaderConstantsPtr,
                shaderLayoutForState,
                effectiveHasTexture,
                textureInfo,
                result.alphaTestEnabled,
                proxyOnly,
                visibleProxy,
                result);
        }

        char __fastcall DetourDrawPrimitiveBuffer(D3DRendererZ* renderer, void*, PrimitiveDraw* draw, int passMode)
        {
            ++g_counters.drawsSeen;

            if (!g_config.enabled || !renderer || !draw) {
                return g_originalDrawPrimitiveBuffer(renderer, draw, passMode);
            }

            IDirect3DDevice9* device = renderer->Device();
            if (!device || !draw->vertexBuffer || draw->primitiveCount == 0 || !IsSupportedPrimitiveType(draw->primitiveType)) {
                ++g_counters.passthroughDraws;
                return g_originalDrawPrimitiveBuffer(renderer, draw, passMode);
            }

            if (g_config.submissionMode == BridgeSubmissionMode::OriginalOnly) {
                ++g_counters.originalOnlyDraws;
                ++g_counters.visibleReplacementBlocked;
                return g_originalDrawPrimitiveBuffer(renderer, draw, passMode);
            }

            if (IsAfterOriginalProxySubmission(g_config.submissionMode)) {
                const bool visibleProxy = IsVisibleProxySubmission(g_config.submissionMode);
                const bool proxyColorWrites = ProxySubmissionUsesColorWrites(g_config.submissionMode);
                ++g_counters.originalOnlyDraws;
                ++g_counters.visibleReplacementBlocked;
                const char originalResult = g_originalDrawPrimitiveBuffer(renderer, draw, passMode);

                if (passMode == 2) {
                    ++g_counters.proxySkippedPassMode;
                    FixedFunctionPassResult skipped{};
                    skipped.proxyOnly = true;
                    skipped.visibleProxy = visibleProxy;
                    skipped.colorWritesEnabled = proxyColorWrites;
                    skipped.noOpBlend = proxyColorWrites && !visibleProxy;
                    skipped.status = FixedFunctionPassStatus::SkippedUi;
                    skipped.hr = S_OK;
                    skipped.reason = FixedFunctionPassReason::SkippedPassMode;
                    SetPassDetail(skipped, "passMode 2 prepass is not mirrored");
                    LogProxyEvent("skip", draw, passMode, skipped);
                } else if (draw->primitiveCount != 0) {
                    ++g_counters.proxyAttempts;
                    FixedFunctionPassResult duplicateResult{};
                    duplicateResult.proxyOnly = true;
                    duplicateResult.visibleProxy = visibleProxy;
                    duplicateResult.colorWritesEnabled = proxyColorWrites;
                    duplicateResult.noOpBlend = proxyColorWrites && !visibleProxy;
                    if (ShouldSkipDuplicateProxySubmission(draw, passMode, duplicateResult)) {
                        ++g_counters.skippedDraws;
                        ++g_counters.uiSkippedDraws;
                        LogProxyEvent("skip", draw, passMode, duplicateResult);
                        return originalResult;
                    }

                    if (draw->shaderConstant) {
                        g_applyShaderConstant(draw->shaderConstant, device);
                    }

                    g_setStream(renderer, draw->vertexBuffer, draw->stride);
                    if (draw->indexBuffer) {
                        g_setIndices(renderer, draw->indexBuffer, draw->indexBindParam);
                    }

                    const FixedFunctionPassResult proxyResult = DrawFixedFunctionPass(device, renderer, draw, true, visibleProxy, proxyColorWrites);
                    switch (proxyResult.status) {
                        case FixedFunctionPassStatus::ConvertedDirect:
                            ++g_counters.proxySubmitted;
                            ++g_counters.proxySubmittedDirect;
                            LogProxyEvent("submitted", draw, passMode, proxyResult);
                            break;

                        case FixedFunctionPassStatus::ConvertedRepacked:
                            ++g_counters.proxySubmitted;
                            ++g_counters.proxySubmittedRepacked;
                            LogProxyEvent("submitted", draw, passMode, proxyResult);
                            break;

                        case FixedFunctionPassStatus::SkippedUi:
                            ++g_counters.skippedDraws;
                            ++g_counters.uiSkippedDraws;
                            LogProxyEvent("skip", draw, passMode, proxyResult);
                            break;

                        default:
                            ++g_counters.unsupportedDraws;
                            LogProxyEvent("unsupported", draw, passMode, proxyResult);
                            break;
                    }
                }

                return originalResult;
            }

            const bool runtimeFixedFunction = g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction;

            if (runtimeFixedFunction && passMode == 2) {
                ++g_counters.skippedDraws;
                ++g_counters.uiSkippedDraws;
                return g_originalDrawPrimitiveBuffer(renderer, draw, passMode);
            }

            if (draw->shaderConstant) {
                g_applyShaderConstant(draw->shaderConstant, device);
            }

            g_setStream(renderer, draw->vertexBuffer, draw->stride);
            if (draw->indexBuffer) {
                g_setIndices(renderer, draw->indexBuffer, draw->indexBindParam);
            }

            if (passMode == 2) {
                return 0;
            }

            int splitCount = g_computeSplitCount(renderer, draw->splitCountKey, passMode);
            while (splitCount > 0) {
                --splitCount;

                if (draw->primitiveCount == 0) {
                    break;
                }

                if (runtimeFixedFunction) {
                    ++g_counters.runtimeAttempts;
                }
                const FixedFunctionPassResult result = DrawFixedFunctionPass(device, renderer, draw, false, false, true);
                switch (result.status) {
                    case FixedFunctionPassStatus::ConvertedDirect:
                        ++g_counters.convertedDraws;
                        ++g_counters.directFvfDraws;
                        if (runtimeFixedFunction) {
                            ++g_counters.runtimeSubmitted;
                        }
                        break;

                    case FixedFunctionPassStatus::ConvertedRepacked:
                        ++g_counters.convertedDraws;
                        ++g_counters.repackedDraws;
                        if (runtimeFixedFunction) {
                            ++g_counters.runtimeSubmitted;
                        }
                        break;

                    case FixedFunctionPassStatus::SkippedUi:
                        ++g_counters.skippedDraws;
                        ++g_counters.uiSkippedDraws;
                        DrawOriginalPass(device, draw);
                        break;

                    default:
                        if (g_config.failOpen) {
                            ++g_counters.unsupportedDraws;
                            DrawOriginalPass(device, draw);
                        } else {
                            ++g_counters.skippedDraws;
                        }
                        break;
                }

                g_advanceDrawPass(renderer);
            }

            return 1;
        }

        char __fastcall DetourRenderCommand(D3DRendererZ* renderer, void*, RenderCommand188* command)
        {
            ++g_counters.commands;
            RenderCommand188* previous = g_currentCommand;
            g_currentCommand = command;
            const char result = g_originalRenderCommand(renderer, command);
            g_currentCommand = previous;
            return result;
        }

        int __fastcall DetourQueueDrain(D3DRendererZ* renderer, void*, int sceneContext)
        {
            ++g_counters.queueDrains;
            return g_originalQueueDrain(renderer, sceneContext);
        }

        int __fastcall DetourDrawPrimitiveUp(D3DRendererZ* renderer, void*, int primitiveType, std::uint16_t primitiveCount, int vertexData, int vertexStride, int splitKey, int passMode)
        {
            ++g_counters.primitiveUpDraws;
            return g_originalDrawPrimitiveUp(renderer, primitiveType, primitiveCount, vertexData, vertexStride, splitKey, passMode);
        }

        int __fastcall DetourMatrixMultiplyHelper(const D3DMATRIX* rhs, void*, D3DMATRIX* out, const D3DMATRIX* lhs)
        {
            const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            const int result = g_originalMatrixMultiplyHelper
                ? g_originalMatrixMultiplyHelper(rhs, out, lhs)
                : 0;

            if (out && (g_config.matrixProbeLog || g_config.debugLog)) {
                RecordMatrixProbeCandidate("matrixMultiply", *out, caller);
            }

            return result;
        }

        int __cdecl DetourLookAtHelper(const void* eye, const void* at, const void* up, D3DMATRIX* out)
        {
            const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            const int result = g_originalLookAtHelper
                ? g_originalLookAtHelper(eye, at, up, out)
                : 0;

            if (out && (g_config.matrixProbeLog || g_config.debugLog)) {
                RecordMatrixProbeCandidate("lookAt", *out, caller);
            }

            return result;
        }

        bool CreateHook(std::uintptr_t address, void* detour, void** original, const char* name)
        {
            const MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(address), detour, original);
            if (status != MH_OK) {
                LogAlways("MH_CreateHook failed for %s: %d", name, static_cast<int>(status));
                return false;
            }

            LogAlways("MH_CreateHook ok for %s at 0x%p", name, reinterpret_cast<void*>(address));
            return true;
        }
    }

    bool Install(const RatataRConfig& cfg)
    {
        g_config.enabled = cfg.rtxRemixBridge;
        g_config.debugLog = cfg.rtxDebugLog;
        g_config.failOpen = cfg.rtxFailOpen;
        g_config.logFile = cfg.rtxBridgeLogFile;
        g_config.allowUnsafeShaderWvp = cfg.rtxAllowUnsafeShaderWvp;
        g_config.allowUnsafeRuntimeReplace = cfg.rtxAllowUnsafeRuntimeReplace;
        g_config.submissionMode = ParseSubmissionMode(cfg.rtxBridgeSubmissionMode);
        g_config.transformMode = ParseTransformMode(cfg.rtxTransformMode);
        g_config.wvpTransposeMode = ParseWvpTransposeMode(cfg.rtxWvpTranspose);
        g_config.vertexBridgeMode = ParseVertexBridgeMode(cfg.rtxVertexBridgeMode);
        g_config.lightBridgeMode = ParseLightBridgeMode(cfg.rtxLightBridgeMode);
        g_config.fixedFunctionMaterialMode = ParseFixedFunctionMaterialMode(cfg.rtxFixedFunctionMaterialMode);
        g_config.shaderMaterialEmissiveScale = cfg.rtxShaderMaterialEmissiveScale;
        g_config.captureAllowTexturelessProxy = cfg.rtxCaptureAllowTexturelessProxy;
        g_config.matrixProbeLog = cfg.rtxMatrixProbeLog;
        g_config.proxyGeometryFilter = ParseProxyGeometryFilterMode(cfg.rtxProxyGeometryFilter);
        g_config.proxyDeduplicateCommandDraws = cfg.rtxProxyDeduplicateCommandDraws;
        g_config.transformAssemblyMode = ParseTransformAssemblyMode(cfg.rtxTransformAssemblyMode);
        g_config.proxyAlphaTestMode = ParseProxyAlphaTestMode(cfg.rtxProxyAlphaTestMode);
        g_config.proxySkinnedMode = ParseProxySkinnedMode(cfg.rtxProxySkinnedMode);
        g_config.proxyCameraLockMode = ParseProxyCameraLockMode(cfg.rtxProxyCameraLockMode);
        g_config.proxyUiSkyMode = ParseProxyUiSkyMode(cfg.rtxProxyUiSkyMode);
        g_config.autoLockWvpRegister = cfg.rtxAutoLockWvpRegister;
        g_config.wvpRegister = ParseWvpRegister(cfg.rtxWvpRegister);
        g_config.repackMaxVertices = cfg.rtxRepackMaxVertices == 0 ? 65536 : cfg.rtxRepackMaxVertices;

        InitializeFileLog();
        LogAlways("bridge log path: %s", g_logPath.empty() ? "(disabled/unavailable)" : g_logPath.c_str());
        const BridgeSubmissionMode requestedSubmissionMode = g_config.submissionMode;
        const RtxTransformMode requestedTransformMode = g_config.transformMode;
        const ProxyAlphaTestMode requestedAlphaTestMode = g_config.proxyAlphaTestMode;

        if (g_config.transformMode == RtxTransformMode::ShaderConstantsWvp) {
            if (g_config.autoLockWvpRegister) {
                LogAlways("shaderConstantsWvp guard: disabling WVP auto-lock");
            }
            g_config.autoLockWvpRegister = false;
            g_lockedWvpValid = false;
        }

        if (g_config.submissionMode == BridgeSubmissionMode::RuntimeFixedFunction &&
            g_config.allowUnsafeRuntimeReplace &&
            g_config.transformMode != RtxTransformMode::RendererMatrixCache) {
            LogAlways(
                "runtimeFixedFunction guard: forcing transform mode rendererMatrixCache from %s",
                TransformModeName(g_config.transformMode));
            g_config.transformMode = RtxTransformMode::RendererMatrixCache;
        }

        SanitizeBridgeConfig(requestedSubmissionMode, requestedTransformMode, requestedAlphaTestMode);

        g_lockedWvpValid = false;
        g_lockedWvpRegister = -1;
        g_lockedWvpTransposed = false;
        g_shaderLightHashValid = false;
        g_shaderLightHash = 0;
        g_shaderLightSetsAppliedThisFrame = 0;
        g_frameCameraCandidateCount = 0;
        g_frameCameraCandidates.fill(FrameCameraLock{});
        g_proxyDedupeCommand = nullptr;
        g_proxyDedupeHashCount = 0;

        LogAlways(
            "bridge init enabled=%d debug=%d failOpen=%d fileLog=%d unsafeShaderWvp=%d unsafeRuntimeReplace=%d submission=%s captureOnlyProxy=%d visibleProxy=%d proxyColorWrite=%d transformMode=%s transformAssembly=%s wvpRegister=%d wvpTranspose=%s autoLock=%d vertexBridge=%s repackMax=%u lightBridge=%s fixedFunctionMaterial=%s shaderEmissiveScale=%g allowTexturelessProxy=%d matrixProbeLog=%d proxyGeometryFilter=%s proxyDedupeCommandDraws=%d proxyAlphaTestMode=%s proxySkinnedMode=%s proxyCameraLockMode=%s proxyUiSkyMode=%s",
            g_config.enabled ? 1 : 0,
            g_config.debugLog ? 1 : 0,
            g_config.failOpen ? 1 : 0,
            g_config.logFile ? 1 : 0,
            g_config.allowUnsafeShaderWvp ? 1 : 0,
            g_config.allowUnsafeRuntimeReplace ? 1 : 0,
            SubmissionModeName(g_config.submissionMode),
            (g_config.submissionMode == BridgeSubmissionMode::ProxyAfterOriginal ||
             g_config.submissionMode == BridgeSubmissionMode::CaptureProxyAfterOriginal) ? 1 : 0,
            IsVisibleProxySubmission(g_config.submissionMode) ? 1 : 0,
            ProxySubmissionUsesColorWrites(g_config.submissionMode) ? 1 : 0,
            TransformModeName(g_config.transformMode),
            TransformAssemblyModeName(g_config.transformAssemblyMode),
            g_config.wvpRegister,
            WvpTransposeModeName(g_config.wvpTransposeMode),
            g_config.autoLockWvpRegister ? 1 : 0,
            VertexBridgeModeName(g_config.vertexBridgeMode),
            g_config.repackMaxVertices,
            LightBridgeModeName(g_config.lightBridgeMode),
            FixedFunctionMaterialModeName(g_config.fixedFunctionMaterialMode),
            g_config.shaderMaterialEmissiveScale,
            g_config.captureAllowTexturelessProxy ? 1 : 0,
            g_config.matrixProbeLog ? 1 : 0,
            ProxyGeometryFilterModeName(g_config.proxyGeometryFilter),
            g_config.proxyDeduplicateCommandDraws ? 1 : 0,
            ProxyAlphaTestModeName(g_config.proxyAlphaTestMode),
            ProxySkinnedModeName(g_config.proxySkinnedMode),
            ProxyCameraLockModeName(g_config.proxyCameraLockMode),
            ProxyUiSkyModeName(g_config.proxyUiSkyMode));
        CheckDxvkConfig();

        if (!g_config.enabled) {
            LogAlways("bridge disabled by config");
            return true;
        }

        if (g_installed) {
            LogAlways("bridge already installed");
            return true;
        }

        if (!ResolveBridgeAddresses()) {
            LogAlways("bridge signatures failed; fixed-function bridge disabled");
            return false;
        }

        LogAlways(
            "bridge addresses queue=0x%p command=0x%p draw=0x%p drawUP=0x%p setStream=0x%p setIndices=0x%p",
            reinterpret_cast<void*>(g_addresses.queueDrain),
            reinterpret_cast<void*>(g_addresses.renderCommand),
            reinterpret_cast<void*>(g_addresses.drawPrimitiveBuffer),
            reinterpret_cast<void*>(g_addresses.drawPrimitiveUp),
            reinterpret_cast<void*>(g_addresses.setStream),
            reinterpret_cast<void*>(g_addresses.setIndices));
        LogAlways(
            "bridge helpers shaderConst=0x%p splitCount=0x%p advancePass=0x%p matrixMultiply=0x%p lookAt=0x%p",
            reinterpret_cast<void*>(g_addresses.applyShaderConstant),
            reinterpret_cast<void*>(g_addresses.computeSplitCount),
            reinterpret_cast<void*>(g_addresses.advanceDrawPass),
            reinterpret_cast<void*>(g_addresses.matrixMultiplyHelper),
            reinterpret_cast<void*>(g_addresses.lookAtHelper));

        g_applyShaderConstant = reinterpret_cast<ApplyShaderConstantFn>(g_addresses.applyShaderConstant);
        g_setStream = reinterpret_cast<SetStreamFn>(g_addresses.setStream);
        g_setIndices = reinterpret_cast<SetIndicesFn>(g_addresses.setIndices);
        g_computeSplitCount = reinterpret_cast<ComputeSplitCountFn>(g_addresses.computeSplitCount);
        g_advanceDrawPass = reinterpret_cast<AdvanceDrawPassFn>(g_addresses.advanceDrawPass);

        bool ok = true;
        ok &= CreateHook(g_addresses.queueDrain, &DetourQueueDrain, reinterpret_cast<void**>(&g_originalQueueDrain), "RenderQueueDrain");
        ok &= CreateHook(g_addresses.renderCommand, &DetourRenderCommand, reinterpret_cast<void**>(&g_originalRenderCommand), "RenderCommand");
        ok &= CreateHook(g_addresses.drawPrimitiveBuffer, &DetourDrawPrimitiveBuffer, reinterpret_cast<void**>(&g_originalDrawPrimitiveBuffer), "DrawPrimitiveBuffer");
        ok &= CreateHook(g_addresses.drawPrimitiveUp, &DetourDrawPrimitiveUp, reinterpret_cast<void**>(&g_originalDrawPrimitiveUp), "DrawPrimitiveUP");
        if ((g_config.matrixProbeLog || g_config.debugLog) && g_addresses.matrixMultiplyHelper) {
            if (!CreateHook(g_addresses.matrixMultiplyHelper, &DetourMatrixMultiplyHelper, reinterpret_cast<void**>(&g_originalMatrixMultiplyHelper), "MatrixMultiplyHelper")) {
                LogAlways("MatrixMultiplyHelper probe hook disabled");
            }
        }
        if ((g_config.matrixProbeLog || g_config.debugLog) && g_addresses.lookAtHelper) {
            if (!CreateHook(g_addresses.lookAtHelper, &DetourLookAtHelper, reinterpret_cast<void**>(&g_originalLookAtHelper), "LookAtHelper")) {
                LogAlways("LookAtHelper probe hook disabled");
            }
        }

        g_installed = ok;
        LogAlways("bridge install %s", ok ? "complete" : "incomplete");
        return ok;
    }

    void OnEndScene()
    {
        if (g_installed) {
            g_shaderLightSetsAppliedThisFrame = 0;
            g_frameCameraCandidateCount = 0;
            g_proxyDedupeCommand = nullptr;
            g_proxyDedupeHashCount = 0;
        }

        if ((!g_config.logFile && !g_config.debugLog) || !g_installed) {
            return;
        }

        static std::uint32_t frameCounter = 0;
        ++frameCounter;
        if ((frameCounter % 120) != 0) {
            return;
        }

        LogAlways(
            "counters queue=%llu commands=%llu seen=%llu originalOnly=%llu replacementBlocked=%llu converted=%llu directFvf=%llu repacked=%llu proxyAttempts=%llu proxySubmitted=%llu proxyDirect=%llu proxyRepacked=%llu proxyPassModeSkip=%llu proxyNoTransform=%llu proxyBadAffine=%llu proxyBadViewProj=%llu proxyBadBounds=%llu proxyZSkip=%llu proxyRhwSkip=%llu proxyScreenSkip=%llu proxyFvfUnsupported=%llu proxyRepackUnsupported=%llu proxyNoTexcoord=%llu proxyNonTriangle=%llu proxyDrawFail=%llu runtimeAttempts=%llu runtimeSubmitted=%llu invalidIndex=%llu badRuntimeTransform=%llu passthrough=%llu unsupported=%llu skipped=%llu uiSkip=%llu rhwSkip=%llu zSkip=%llu screenMatrixSkip=%llu up=%llu fvfFail=%llu repackFallback=%llu noWorldMatrixFallback=%llu noTexcoordFallback=%llu nonTriangleFallback=%llu matrixCandidates=%llu matrixFail=%llu shaderWvpCandidates=%llu shaderWvpSelected=%llu shaderWvpRejectLogs=%llu matrixProbeCaptured=%llu matrixProbeSelected=%llu matrixProbeRejected=%llu wvpNoCandidate=%llu wvpFallbackWorld=%llu debugLights=%llu wvpMissing=%llu",
            static_cast<unsigned long long>(g_counters.queueDrains),
            static_cast<unsigned long long>(g_counters.commands),
            static_cast<unsigned long long>(g_counters.drawsSeen),
            static_cast<unsigned long long>(g_counters.originalOnlyDraws),
            static_cast<unsigned long long>(g_counters.visibleReplacementBlocked),
            static_cast<unsigned long long>(g_counters.convertedDraws),
            static_cast<unsigned long long>(g_counters.directFvfDraws),
            static_cast<unsigned long long>(g_counters.repackedDraws),
            static_cast<unsigned long long>(g_counters.proxyAttempts),
            static_cast<unsigned long long>(g_counters.proxySubmitted),
            static_cast<unsigned long long>(g_counters.proxySubmittedDirect),
            static_cast<unsigned long long>(g_counters.proxySubmittedRepacked),
            static_cast<unsigned long long>(g_counters.proxySkippedPassMode),
            static_cast<unsigned long long>(g_counters.proxySkippedNoTransform),
            static_cast<unsigned long long>(g_counters.proxySkippedBadAffine),
            static_cast<unsigned long long>(g_counters.proxySkippedBadViewProj),
            static_cast<unsigned long long>(g_counters.proxySkippedBadBounds),
            static_cast<unsigned long long>(g_counters.proxySkippedZDisabled),
            static_cast<unsigned long long>(g_counters.proxySkippedRhw),
            static_cast<unsigned long long>(g_counters.proxySkippedScreenMatrix),
            static_cast<unsigned long long>(g_counters.proxyUnsupportedFvf),
            static_cast<unsigned long long>(g_counters.proxyUnsupportedRepack),
            static_cast<unsigned long long>(g_counters.proxySkippedNoTexcoord),
            static_cast<unsigned long long>(g_counters.proxySkippedNonTriangle),
            static_cast<unsigned long long>(g_counters.proxyDrawFailures),
            static_cast<unsigned long long>(g_counters.runtimeAttempts),
            static_cast<unsigned long long>(g_counters.runtimeSubmitted),
            static_cast<unsigned long long>(g_counters.invalidIndexRanges),
            static_cast<unsigned long long>(g_counters.badRuntimeTransforms),
            static_cast<unsigned long long>(g_counters.passthroughDraws),
            static_cast<unsigned long long>(g_counters.unsupportedDraws),
            static_cast<unsigned long long>(g_counters.skippedDraws),
            static_cast<unsigned long long>(g_counters.uiSkippedDraws),
            static_cast<unsigned long long>(g_counters.rhwSkippedDraws),
            static_cast<unsigned long long>(g_counters.zDisabledSkippedDraws),
            static_cast<unsigned long long>(g_counters.screenMatrixSkips),
            static_cast<unsigned long long>(g_counters.primitiveUpDraws),
            static_cast<unsigned long long>(g_counters.fvfFailures),
            static_cast<unsigned long long>(g_counters.repackFallbacks),
            static_cast<unsigned long long>(g_counters.noWorldMatrixFallbacks),
            static_cast<unsigned long long>(g_counters.noTexcoordFallbacks),
            static_cast<unsigned long long>(g_counters.nonTriangleFallbacks),
            static_cast<unsigned long long>(g_counters.matrixCandidates),
            static_cast<unsigned long long>(g_counters.matrixFailures),
            static_cast<unsigned long long>(g_counters.shaderWvpCandidates),
            static_cast<unsigned long long>(g_counters.shaderWvpSelected),
            static_cast<unsigned long long>(g_counters.shaderWvpRejectedLogs),
            static_cast<unsigned long long>(g_counters.matrixProbeCaptured),
            static_cast<unsigned long long>(g_counters.matrixProbeSelected),
            static_cast<unsigned long long>(g_counters.matrixProbeRejected),
            static_cast<unsigned long long>(g_counters.wvpNoCandidate),
            static_cast<unsigned long long>(g_counters.wvpFallbackWorld),
            static_cast<unsigned long long>(g_counters.debugLightDraws),
            static_cast<unsigned long long>(g_counters.commandWvpMissing));

        LogAlways(
            "capture counters ratCandidates=%llu ratSelected=%llu ratRejected=%llu shaderMaterials=%llu shaderLightsSeen=%llu shaderLightsSubmitted=%llu shaderDir=%llu shaderPoint=%llu shaderSpot=%llu shaderLightSetsApplied=%llu shaderLightSetsChanged=%llu shaderLightSetsReused=%llu shaderLightsSuppressed=%llu normalsCopied=%llu normalsGenerated=%llu normalsDefaulted=%llu",
            static_cast<unsigned long long>(g_counters.ratTransformCandidates),
            static_cast<unsigned long long>(g_counters.ratTransformSelected),
            static_cast<unsigned long long>(g_counters.ratTransformRejected),
            static_cast<unsigned long long>(g_counters.shaderMaterialDraws),
            static_cast<unsigned long long>(g_counters.shaderLightsSeen),
            static_cast<unsigned long long>(g_counters.shaderLegacyLightsSubmitted),
            static_cast<unsigned long long>(g_counters.shaderDirectionalLights),
            static_cast<unsigned long long>(g_counters.shaderPointLights),
            static_cast<unsigned long long>(g_counters.shaderSpotLights),
            static_cast<unsigned long long>(g_counters.shaderLightSetsApplied),
            static_cast<unsigned long long>(g_counters.shaderLightSetsChanged),
            static_cast<unsigned long long>(g_counters.shaderLightSetsReused),
            static_cast<unsigned long long>(g_counters.shaderLightsSuppressed),
            static_cast<unsigned long long>(g_counters.repackNormalsCopied),
            static_cast<unsigned long long>(g_counters.repackNormalsGenerated),
            static_cast<unsigned long long>(g_counters.repackNormalsDefaulted));

        LogAlways(
            "proxy filter counters skinned=%llu patch=%llu transparent=%llu screenCopy=%llu screenPlane=%llu projectedOversize=%llu duplicateCommand=%llu cameraMismatch=%llu alphaSubmitted=%llu alphaRejected=%llu skinnedSubmitted=%llu skinnedPaletteMissing=%llu uiProxySubmitted=%llu skyProxySubmitted=%llu cameraAccepted=%llu cameraRejected=%llu geometryFilter=%s transformAssembly=%s alphaTestMode=%s skinnedMode=%s cameraMode=%s uiSkyMode=%s",
            static_cast<unsigned long long>(g_counters.proxySkippedSkinned),
            static_cast<unsigned long long>(g_counters.proxySkippedPatch),
            static_cast<unsigned long long>(g_counters.proxySkippedTransparent),
            static_cast<unsigned long long>(g_counters.proxySkippedScreenCopy),
            static_cast<unsigned long long>(g_counters.proxySkippedScreenPlane),
            static_cast<unsigned long long>(g_counters.proxySkippedProjectedOversize),
            static_cast<unsigned long long>(g_counters.proxySkippedDuplicateCommand),
            static_cast<unsigned long long>(g_counters.proxySkippedCameraMismatch),
            static_cast<unsigned long long>(g_counters.proxyAlphaTestSubmitted),
            static_cast<unsigned long long>(g_counters.proxyAlphaTestRejected),
            static_cast<unsigned long long>(g_counters.skinnedSubmitted),
            static_cast<unsigned long long>(g_counters.skinnedPaletteMissing),
            static_cast<unsigned long long>(g_counters.uiProxySubmitted),
            static_cast<unsigned long long>(g_counters.skyProxySubmitted),
            static_cast<unsigned long long>(g_counters.cameraCandidateAccepted),
            static_cast<unsigned long long>(g_counters.cameraCandidateRejected),
            ProxyGeometryFilterModeName(g_config.proxyGeometryFilter),
            TransformAssemblyModeName(g_config.transformAssemblyMode),
            ProxyAlphaTestModeName(g_config.proxyAlphaTestMode),
            ProxySkinnedModeName(g_config.proxySkinnedMode),
            ProxyCameraLockModeName(g_config.proxyCameraLockMode),
            ProxyUiSkyModeName(g_config.proxyUiSkyMode));

        LogAlways(
            "renderer triplet counters valid=%llu invalid=%llu submitted=%llu maskedWorld=%llu uiRhw=%llu uiOrtho=%llu uiRejected=%llu skyWorld=%llu skyRejected=%llu cameraRelativeRejected=%llu skinPaletteComplete=%llu skinPaletteIncomplete=%llu",
            static_cast<unsigned long long>(g_counters.rendererTripletValid),
            static_cast<unsigned long long>(g_counters.rendererTripletInvalid),
            static_cast<unsigned long long>(g_counters.rendererTripletSubmitted),
            static_cast<unsigned long long>(g_counters.maskedWorldSubmitted),
            static_cast<unsigned long long>(g_counters.uiRhwSubmitted),
            static_cast<unsigned long long>(g_counters.uiOrthographicSubmitted),
            static_cast<unsigned long long>(g_counters.uiHeuristicRejected),
            static_cast<unsigned long long>(g_counters.skyWorldSubmitted),
            static_cast<unsigned long long>(g_counters.skyHeuristicRejected),
            static_cast<unsigned long long>(g_counters.cameraRelativeRejected),
            static_cast<unsigned long long>(g_counters.skinnedPaletteComplete),
            static_cast<unsigned long long>(g_counters.skinnedPaletteIncomplete));

        static BridgeCounters previousCounters{};
        auto delta = [](std::uint64_t current, std::uint64_t previous) -> std::uint64_t {
            return current >= previous ? current - previous : current;
        };

        struct ProxySkipCandidate
        {
            const char* name;
            std::uint64_t count;
        };

        ProxySkipCandidate skipCandidates[] = {
            { "passMode2", delta(g_counters.proxySkippedPassMode, previousCounters.proxySkippedPassMode) },
            { "noTransform", delta(g_counters.proxySkippedNoTransform, previousCounters.proxySkippedNoTransform) },
            { "badAffine", delta(g_counters.proxySkippedBadAffine, previousCounters.proxySkippedBadAffine) },
            { "badViewProjection", delta(g_counters.proxySkippedBadViewProj, previousCounters.proxySkippedBadViewProj) },
            { "badVertexBounds", delta(g_counters.proxySkippedBadBounds, previousCounters.proxySkippedBadBounds) },
            { "wvpNoCandidate", delta(g_counters.wvpNoCandidate, previousCounters.wvpNoCandidate) },
            { "invalidIndex", delta(g_counters.invalidIndexRanges, previousCounters.invalidIndexRanges) },
            { "badRuntimeTransform", delta(g_counters.badRuntimeTransforms, previousCounters.badRuntimeTransforms) },
            { "zDisabled", delta(g_counters.proxySkippedZDisabled, previousCounters.proxySkippedZDisabled) },
            { "rhw", delta(g_counters.proxySkippedRhw, previousCounters.proxySkippedRhw) },
            { "screenMatrix", delta(g_counters.proxySkippedScreenMatrix, previousCounters.proxySkippedScreenMatrix) },
            { "fvfUnsupported", delta(g_counters.proxyUnsupportedFvf, previousCounters.proxyUnsupportedFvf) },
            { "repackUnsupported", delta(g_counters.proxyUnsupportedRepack, previousCounters.proxyUnsupportedRepack) },
            { "missingTexcoord", delta(g_counters.proxySkippedNoTexcoord, previousCounters.proxySkippedNoTexcoord) },
            { "nonTriangle", delta(g_counters.proxySkippedNonTriangle, previousCounters.proxySkippedNonTriangle) },
            { "skinned", delta(g_counters.proxySkippedSkinned, previousCounters.proxySkippedSkinned) },
            { "patch", delta(g_counters.proxySkippedPatch, previousCounters.proxySkippedPatch) },
            { "transparent", delta(g_counters.proxySkippedTransparent, previousCounters.proxySkippedTransparent) },
            { "screenCopy", delta(g_counters.proxySkippedScreenCopy, previousCounters.proxySkippedScreenCopy) },
            { "screenPlane", delta(g_counters.proxySkippedScreenPlane, previousCounters.proxySkippedScreenPlane) },
            { "projectedOversize", delta(g_counters.proxySkippedProjectedOversize, previousCounters.proxySkippedProjectedOversize) },
            { "duplicateCommand", delta(g_counters.proxySkippedDuplicateCommand, previousCounters.proxySkippedDuplicateCommand) },
            { "cameraMismatch", delta(g_counters.proxySkippedCameraMismatch, previousCounters.proxySkippedCameraMismatch) },
            { "drawFailed", delta(g_counters.proxyDrawFailures, previousCounters.proxyDrawFailures) },
        };

        const ProxySkipCandidate* topSkip = &skipCandidates[0];
        for (const ProxySkipCandidate& candidate : skipCandidates) {
            if (candidate.count > topSkip->count) {
                topSkip = &candidate;
            }
        }

        const std::uint64_t intervalAttempts = delta(g_counters.proxyAttempts, previousCounters.proxyAttempts);
        const std::uint64_t intervalSubmitted = delta(g_counters.proxySubmitted, previousCounters.proxySubmitted);
        const std::uint64_t intervalDirect = delta(g_counters.proxySubmittedDirect, previousCounters.proxySubmittedDirect);
        const std::uint64_t intervalRepacked = delta(g_counters.proxySubmittedRepacked, previousCounters.proxySubmittedRepacked);
        LogAlways(
            "proxy interval frames=120 attempts=%llu submitted=%llu direct=%llu repacked=%llu topSkip=%s:%llu",
            static_cast<unsigned long long>(intervalAttempts),
            static_cast<unsigned long long>(intervalSubmitted),
            static_cast<unsigned long long>(intervalDirect),
            static_cast<unsigned long long>(intervalRepacked),
            topSkip->name,
            static_cast<unsigned long long>(topSkip->count));

        previousCounters = g_counters;
    }
}

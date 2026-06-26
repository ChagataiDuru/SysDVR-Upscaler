#include "app/AppConfig.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace ns60 {
namespace {
bool parsePositiveInt(std::string_view text, int& value) {
    int parsed{}; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),parsed);
    if(error!=std::errc{}||end!=text.data()+text.size()||parsed<=0||parsed>16384) return false; value=parsed; return true;
}

bool parseIntInRange(std::string_view text, int& value, int minimum, int maximum) {
    int parsed{}; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),parsed);
    if(error!=std::errc{}||end!=text.data()+text.size()||parsed<minimum||parsed>maximum) return false; value=parsed; return true;
}

bool parseNonNegativeInt(std::string_view text, int& value) {
    int parsed{}; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),parsed);
    if(error!=std::errc{}||end!=text.data()+text.size()||parsed<0||parsed>256) return false; value=parsed; return true;
}

bool parseUnitFloat(std::string_view text, float& value) {
    float parsed{}; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),parsed);
    if(error!=std::errc{}||end!=text.data()+text.size()||!validSharpness(parsed)) return false; value=parsed; return true;
}
std::optional<bool> parseBoolean(std::string_view text) { if(text=="on") return true; if(text=="off") return false; return std::nullopt; }
std::optional<LogLevel> parseLogLevel(std::string_view text) {
    if(text=="trace")return LogLevel::Trace; if(text=="debug")return LogLevel::Debug; if(text=="info")return LogLevel::Info;
    if(text=="warning"||text=="warn")return LogLevel::Warning; if(text=="error")return LogLevel::Error;
    if(text=="critical")return LogLevel::Critical; return std::nullopt;
}
std::optional<SourceKind> parseSourceKind(std::string_view text) {
    if(text=="file") return SourceKind::File;
    if(text=="sysdvr-pipe") return SourceKind::SysDvrPipe;
    if(text=="sysdvr") return SourceKind::SysDvr;
    return std::nullopt;
}
std::optional<LatencyProfile> parseLatencyProfile(std::string_view text) {
    if(text=="quality") return LatencyProfile::Quality;
    if(text=="balanced") return LatencyProfile::Balanced;
    if(text=="ultra") return LatencyProfile::Ultra;
    return std::nullopt;
}
void applyLatencyProfile(LatencyProfile profile, AppConfig& config) {
    config.latencyProfile = profile;
    switch(profile) {
    case LatencyProfile::Quality:
        config.liveFrameQueueDepth = 2;
        config.bridgePipeQueueMessages = 32;
        config.bridgePipeQueueBytes = 2 * 1024 * 1024;
        config.bridgePipeMaxAgeMs = 100;
        break;
    case LatencyProfile::Balanced:
        config.liveFrameQueueDepth = 1;
        config.bridgePipeQueueMessages = 16;
        config.bridgePipeQueueBytes = 1024 * 1024;
        config.bridgePipeMaxAgeMs = 50;
        break;
    case LatencyProfile::Ultra:
        config.liveFrameQueueDepth = 1;
        config.bridgePipeQueueMessages = 8;
        config.bridgePipeQueueBytes = 1024 * 1024;
        config.bridgePipeMaxAgeMs = 33;
        break;
    }
}
std::filesystem::path utf8Path(std::string_view text) { const auto* first=reinterpret_cast<const char8_t*>(text.data()); return std::filesystem::path(std::u8string(first,first+text.size())); }

bool applyQualityPreset(std::string_view text, AppConfig& config) {
    if(text=="balanced") {
        config.upscale=UpscaleMode::Fsr1EasuRcas;
        config.sharpen.casSharpness=0.35F;
        config.sharpen.rcasSharpness=0.25F;
        config.antiRinging=true;
        config.chromaUpscale=ChromaUpscaleMode::BicubicCatmullRom;
        config.finalFilter=FinalFilter::Bilinear;
        return true;
    }
    if(text=="performance") {
        config.upscale=UpscaleMode::BilinearCas;
        config.sharpen.casSharpness=0.30F;
        config.antiRinging=true;
        config.chromaUpscale=ChromaUpscaleMode::Bilinear;
        config.finalFilter=FinalFilter::Bilinear;
        return true;
    }
    if(text=="quality") {
        config.upscale=UpscaleMode::Fsr1EasuRcas;
        config.sharpen.casSharpness=0.35F;
        config.sharpen.rcasSharpness=0.20F;
        config.antiRinging=true;
        config.chromaUpscale=ChromaUpscaleMode::Lanczos2;
        config.finalFilter=FinalFilter::Bilinear;
        return true;
    }
    return false;
}
}

ParseResult parseCommandLine(const std::vector<std::string>& args, bool defaultValidation) {
    AppConfig config; config.validation=defaultValidation; applyLatencyProfile(config.latencyProfile, config); bool inputSeen=false;
    for(std::size_t index=1;index<args.size();++index) {
        const auto& argument=args[index];
        auto requireValue=[&](std::string_view)->const std::string*{if(index+1>=args.size())return nullptr;++index;return &args[index];};
        if(argument=="--help"||argument=="-h")return{ParseAction::Help,std::nullopt,{}};
        if(argument=="--version")return{ParseAction::Version,std::nullopt,{}};
        if(argument=="--loop"){config.loop=true;continue;} if(argument=="--fullscreen"){config.fullscreen=true;continue;}
        if(argument=="--borderless"){config.borderless=true;continue;} if(argument=="--drop-late-frames"){config.dropLateFrames=true;continue;}
        if(argument=="--source") { const auto* value=requireValue(argument);const auto parsed=value?parseSourceKind(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"--source requires file, sysdvr-pipe, or sysdvr"};config.source=*parsed; }
        else if(argument=="--input") { const auto* value=requireValue(argument); if(!value||value->empty())return{ParseAction::Run,std::nullopt,"--input requires a path"}; config.input=utf8Path(*value);inputSeen=true; }
        else if(argument=="--pipe-name") { const auto* value=requireValue(argument); if(!value||value->empty())return{ParseAction::Run,std::nullopt,"--pipe-name requires a non-empty pipe name"}; config.pipeName=*value; }
        else if(argument=="--sysdvr-bridge") { const auto* value=requireValue(argument); if(!value||value->empty())return{ParseAction::Run,std::nullopt,"--sysdvr-bridge requires a path"}; config.sysdvrBridge=utf8Path(*value); }
        else if(argument=="--latency-profile") { const auto* value=requireValue(argument);const auto parsed=value?parseLatencyProfile(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"--latency-profile requires quality, balanced, or ultra"};applyLatencyProfile(*parsed, config); }
        else if(argument=="--live-frame-queue-depth") { const auto* value=requireValue(argument);int parsed{};if(!value||!parseIntInRange(*value,parsed,1,3))return{ParseAction::Run,std::nullopt,"--live-frame-queue-depth requires an integer in [1, 3]"};config.liveFrameQueueDepth=parsed; }
        else if(argument=="--upscaler-pipe-queue-messages") { const auto* value=requireValue(argument);int parsed{};if(!value||!parseIntInRange(*value,parsed,1,1024))return{ParseAction::Run,std::nullopt,"--upscaler-pipe-queue-messages requires an integer in [1, 1024]"};config.bridgePipeQueueMessages=parsed; }
        else if(argument=="--upscaler-pipe-queue-bytes") { const auto* value=requireValue(argument);int parsed{};if(!value||!parseIntInRange(*value,parsed,64*1024,64*1024*1024))return{ParseAction::Run,std::nullopt,"--upscaler-pipe-queue-bytes requires an integer in [65536, 67108864]"};config.bridgePipeQueueBytes=parsed; }
        else if(argument=="--upscaler-pipe-max-age-ms") { const auto* value=requireValue(argument);int parsed{};if(!value||!parseIntInRange(*value,parsed,1,1000))return{ParseAction::Run,std::nullopt,"--upscaler-pipe-max-age-ms requires an integer in [1, 1000]"};config.bridgePipeMaxAgeMs=parsed; }
        else if(argument=="--quality-preset") { const auto* value=requireValue(argument); if(!value||!applyQualityPreset(*value,config))return{ParseAction::Run,std::nullopt,"--quality-preset requires balanced, performance, or quality"}; }
        else if(argument=="--width"||argument=="--height") { const auto* value=requireValue(argument);int parsed{};if(!value||!parsePositiveInt(*value,parsed))return{ParseAction::Run,std::nullopt,argument+" requires an integer in [1, 16384]"};(argument=="--width"?config.outputWidth:config.outputHeight)=parsed; }
        else if(argument=="--monitor") { const auto* value=requireValue(argument);int parsed{};if(!value||!parseNonNegativeInt(*value,parsed))return{ParseAction::Run,std::nullopt,"--monitor requires a non-negative integer monitor index"};config.monitorIndex=parsed; }
        else if(argument=="--upscale") { const auto* value=requireValue(argument);const auto parsed=value?parseUpscaleMode(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"Invalid --upscale mode (nearest, bilinear, bicubic, lanczos2, bilinear-cas, lanczos2-cas, fsr1-easu, fsr1-easu-rcas)"};config.upscale=*parsed; }
        else if(argument=="--cas-sharpness"||argument=="--rcas-sharpness") { const auto* value=requireValue(argument);float parsed{};if(!value||!parseUnitFloat(*value,parsed))return{ParseAction::Run,std::nullopt,argument+" requires a finite number in [0.0, 1.0]"};(argument=="--cas-sharpness"?config.sharpen.casSharpness:config.sharpen.rcasSharpness)=parsed; }
        else if(argument=="--anti-ringing"||argument=="--vsync"||argument=="--validation") { const auto* value=requireValue(argument);const auto parsed=value?parseBoolean(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,argument+" requires 'on' or 'off'"};if(argument=="--anti-ringing")config.antiRinging=*parsed;else(argument=="--vsync"?config.vsync:config.validation)=*parsed; }
        else if(argument=="--compare") { const auto* value=requireValue(argument);const auto comma=value?value->find(','):std::string::npos;if(!value||comma==std::string::npos||comma==0||comma+1>=value->size())return{ParseAction::Run,std::nullopt,"--compare requires <modeA,modeB>"};const auto a=parseUpscaleMode(std::string_view(*value).substr(0,comma));const auto b=parseUpscaleMode(std::string_view(*value).substr(comma+1));if(!a||!b)return{ParseAction::Run,std::nullopt,"--compare contains an invalid upscale mode"};config.comparisonA=*a;config.comparisonB=*b; }
        else if(argument=="--presentation") { const auto* value=requireValue(argument);const auto parsed=value?parsePresentationMode(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"--presentation requires exact, fit, fill, or integer"};config.presentation=*parsed;config.presentationExplicit=true; }
        else if(argument=="--final-filter") { const auto* value=requireValue(argument);const auto parsed=value?parseFinalFilter(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"--final-filter requires nearest or bilinear"};config.finalFilter=*parsed; }
        else if(argument=="--chroma-upscale") { const auto* value=requireValue(argument);const auto parsed=value?parseChromaUpscaleMode(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"--chroma-upscale requires bilinear, bicubic, lanczos2, or edge-aware"};config.chromaUpscale=*parsed; }
        else if(argument=="--log-level") { const auto* value=requireValue(argument);const auto parsed=value?parseLogLevel(*value):std::nullopt;if(!parsed)return{ParseAction::Run,std::nullopt,"Invalid --log-level (trace, debug, info, warning, error, critical)"};config.logLevel=*parsed; }
        else if(!argument.empty()&&argument.front()!='-'&&!inputSeen){config.input=utf8Path(argument);inputSeen=true;}
        else return{ParseAction::Run,std::nullopt,"Unknown option: "+argument};
    }
    if(config.source==SourceKind::File&&!inputSeen)return{ParseAction::Run,std::nullopt,"Missing required --input <path>"};
    if(config.source!=SourceKind::File&&inputSeen)return{ParseAction::Run,std::nullopt,"--input is only valid with --source file"};
    if(config.source!=SourceKind::File&&config.loop)return{ParseAction::Run,std::nullopt,"--loop is only valid with file input"};
    if(config.source==SourceKind::SysDvr&&config.sysdvrBridge.empty())return{ParseAction::Run,std::nullopt,"--source sysdvr requires --sysdvr-bridge <path>"};
    if(config.pipeName.empty())return{ParseAction::Run,std::nullopt,"--pipe-name requires a non-empty pipe name"};
    if(config.borderless)config.fullscreen=true;
    return{ParseAction::Run,std::move(config),{}};
}

std::string commandLineHelp() { return R"(NexusStream60 - offline and live SysDVR Vulkan upscaling laboratory

Usage:
  NexusStream60.exe --input <video.mp4> [options]
  NexusStream60.exe --source sysdvr-pipe --pipe-name <name> [options]
  NexusStream60.exe --source sysdvr --sysdvr-bridge <SysDVR-Client.exe> [options]

Input options:
  --source <mode>             file|sysdvr-pipe|sysdvr (default file)
  --input <path>              Input MP4 for file mode (a positional path is also accepted)
  --pipe-name <name>          Named pipe for sysdvr-pipe mode (default SysDVR-Upscaler.Video)
  --sysdvr-bridge <path>      SysDVR-Client executable for unified sysdvr mode
  --latency-profile <profile> quality|balanced|ultra live buffering preset (default balanced)
  --live-frame-queue-depth <n> Live decoded queue depth in [1,3] (default 1)
  --upscaler-pipe-queue-messages <n> Managed bridge queue message cap
  --upscaler-pipe-queue-bytes <n> Managed bridge queue byte cap
  --upscaler-pipe-max-age-ms <n> Managed bridge oldest-payload age cap

Output and quality:
  --width/--height <pixels>   Reconstruction output/client framebuffer request (default 1920x1080)
  --quality-preset <preset>   balanced|performance|quality
  --monitor <index>           Target monitor for fullscreen/borderless presentation
  --upscale <mode>            nearest|bilinear|bicubic|lanczos2|bilinear-cas|lanczos2-cas|fsr1-easu|fsr1-easu-rcas
  --cas-sharpness <0..1>      FidelityFX CAS strength (default 0.35)
  --rcas-sharpness <0..1>     FidelityFX RCAS strength (default 0.25)
  --anti-ringing <on|off>     Classical-filter anti-ringing clamp (default on)
  --compare <modeA,modeB>     Start split comparison
  --presentation <mode>       exact|fit|fill|integer (default auto: exact at 1:1, fit otherwise)
  --final-filter <filter>     nearest|bilinear for active Fit/Fill/fallback resampling
  --chroma-upscale <mode>     bilinear|bicubic|lanczos2|edge-aware (default bicubic)
  --loop --fullscreen --borderless --drop-late-frames
  --vsync <on|off> --validation <on|off> --log-level <level>
  --help --version
)"; }
const char* toString(LogLevel level) noexcept { switch(level){case LogLevel::Trace:return"TRACE";case LogLevel::Debug:return"DEBUG";case LogLevel::Info:return"INFO";case LogLevel::Warning:return"WARNING";case LogLevel::Error:return"ERROR";case LogLevel::Critical:return"CRITICAL";}return"UNKNOWN"; }
std::string_view toString(SourceKind source) noexcept { switch(source){case SourceKind::File:return"file";case SourceKind::SysDvrPipe:return"sysdvr-pipe";case SourceKind::SysDvr:return"sysdvr";}return"unknown"; }
std::string_view toString(PlaybackPolicy policy) noexcept { switch(policy){case PlaybackPolicy::TimedFile:return"TimedFile";case PlaybackPolicy::ImmediateLive:return"ImmediateLive";}return"unknown"; }
std::string_view toString(LatencyProfile profile) noexcept { switch(profile){case LatencyProfile::Quality:return"quality";case LatencyProfile::Balanced:return"balanced";case LatencyProfile::Ultra:return"ultra";}return"unknown"; }
PlaybackPolicy playbackPolicyFor(SourceKind source) noexcept { return source == SourceKind::File ? PlaybackPolicy::TimedFile : PlaybackPolicy::ImmediateLive; }
} // namespace ns60

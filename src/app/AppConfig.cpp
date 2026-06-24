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
std::filesystem::path utf8Path(std::string_view text) { const auto* first=reinterpret_cast<const char8_t*>(text.data()); return std::filesystem::path(std::u8string(first,first+text.size())); }
}

ParseResult parseCommandLine(const std::vector<std::string>& args, bool defaultValidation) {
    AppConfig config; config.validation=defaultValidation; bool inputSeen=false;
    for(std::size_t index=1;index<args.size();++index) {
        const auto& argument=args[index];
        auto requireValue=[&](std::string_view)->const std::string*{if(index+1>=args.size())return nullptr;++index;return &args[index];};
        if(argument=="--help"||argument=="-h")return{ParseAction::Help,std::nullopt,{}};
        if(argument=="--version")return{ParseAction::Version,std::nullopt,{}};
        if(argument=="--loop"){config.loop=true;continue;} if(argument=="--fullscreen"){config.fullscreen=true;continue;}
        if(argument=="--borderless"){config.borderless=true;continue;} if(argument=="--drop-late-frames"){config.dropLateFrames=true;continue;}
        if(argument=="--input") { const auto* value=requireValue(argument); if(!value||value->empty())return{ParseAction::Run,std::nullopt,"--input requires a path"}; config.input=utf8Path(*value);inputSeen=true; }
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
    if(!inputSeen)return{ParseAction::Run,std::nullopt,"Missing required --input <path>"}; if(config.borderless)config.fullscreen=true;
    return{ParseAction::Run,std::move(config),{}};
}

std::string commandLineHelp() { return R"(NexusStream60 - offline SysDVR Vulkan upscaling laboratory

Usage: NexusStream60.exe --input <video.mp4> [options]

  --input <path>              Input MP4 (a positional path is also accepted)
  --width/--height <pixels>   Reconstruction output/client framebuffer request (default 1920x1080)
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
} // namespace ns60
#include "vk_shaders.h"

#include <StandAlone/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>

namespace vk {

namespace {

std::once_flag g_init;

}  // namespace

bool CompileHlsl(const std::string& src, Stage stage, const char* entry, std::vector<uint32_t>& spirv, std::string& log) {
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    const bool vs = stage == Stage::Vertex;
    EShLanguage lang = vs ? EShLangVertex : EShLangFragment;
    glslang::TShader sh(lang);
    const char* text = src.c_str();
    int len = int(src.size());
    sh.setStringsWithLengths(&text, &len, 1);
    sh.setEntryPoint(entry);
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, vs ? kBindVsUbo : kBindPsUbo);
    sh.setShiftBinding(glslang::EResTexture, vs ? kBindVsTex : kBindPsTex);
    sh.setShiftBinding(glslang::EResSampler, vs ? kBindVsSmp : kBindPsSmp);
    EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
    if (!sh.parse(&glslang::DefaultTBuiltInResource, 100, false, msg)) {
        log = std::string(sh.getInfoLog()) + sh.getInfoDebugLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msg) || !prog.mapIO()) {
        log = std::string(prog.getInfoLog()) + prog.getInfoDebugLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.validate = false;
    opt.disableOptimizer = true;
    spirv.clear();
    glslang::GlslangToSpv(*prog.getIntermediate(lang), spirv, &opt);
    return !spirv.empty();
}

}  // namespace vk

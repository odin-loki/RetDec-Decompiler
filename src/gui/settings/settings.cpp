/**
 * @file src/gui/settings/settings.cpp
 * @brief AppSettings implementation — QSettings-backed persistence.
 */

#include "retdec/gui/settings/settings.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace retdec::gui {

using ::QSettings;

// ─── Singleton ───────────────────────────────────────────────────────────────

AppSettings& AppSettings::instance() {
    static AppSettings s;
    return s;
}

AppSettings::AppSettings() {}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static QSettings makeQSettings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     "retdec", "settings");
}

// ─── Load ─────────────────────────────────────────────────────────────────────

void AppSettings::load() {
    QSettings s = makeQSettings();
    loadGeneral (s);
    loadAnalysis(s);
    loadCUDA    (s);
    loadML      (s);
    loadRecovery(s);
    loadAdvanced(s);
    loadPlugins   (s);
    loadDecompiler(s);
}

void AppSettings::loadGeneral(QSettings& s) {
    s.beginGroup("General");
    general.theme = static_cast<GeneralSettings::Theme>(
        s.value("theme", static_cast<int>(GeneralSettings::Theme::Dark)).toInt());
    QFont font;
    font.fromString(s.value("font", general.editorFont.toString()).toString());
    general.editorFont    = font;
    general.fontSize      = s.value("fontSize", 10).toInt();
    general.language      = s.value("language", "en").toString();
    general.showLineNumbers = s.value("showLineNumbers", true).toBool();
    general.wordWrap      = s.value("wordWrap", false).toBool();
    general.restoreSession = s.value("restoreSession", true).toBool();
    general.lastOpenDir   = s.value("lastOpenDir").toString();
    general.lastBinaryPath = s.value("lastBinaryPath").toString();
    s.endGroup();
}

void AppSettings::loadAnalysis(QSettings& s) {
    s.beginGroup("Analysis");
    analysis.enableTyping          = s.value("enableTyping",       true).toBool();
    analysis.enablePatternMatch    = s.value("enablePatternMatch", true).toBool();
    analysis.enableConcurrency     = s.value("enableConcurrency",  true).toBool();
    analysis.enableCudaRecovery    = s.value("enableCudaRecovery", true).toBool();
    analysis.enableSerialDetect    = s.value("enableSerialDetect", true).toBool();
    analysis.enableModuleCluster   = s.value("enableModuleCluster",true).toBool();
    analysis.enableCxxLifter       = s.value("enableCxxLifter",    true).toBool();
    analysis.minTypeConfidence     = s.value("minTypeConf",   0.5).toDouble();
    analysis.minPatternConfidence  = s.value("minPatternConf",0.6).toDouble();
    analysis.minRecoveryConfidence = s.value("minRecovConf",  0.4).toDouble();
    analysis.maxAnalysisTimeSecs   = s.value("maxTimeSecs",   300).toInt();
    analysis.threadCount           = s.value("threadCount",   0).toInt();
    s.endGroup();
}

void AppSettings::loadCUDA(QSettings& s) {
    s.beginGroup("CUDA");
    cuda.deviceName      = s.value("deviceName").toString();
    cuda.deviceIndex     = s.value("deviceIndex",   0).toInt();
    cuda.kernelCacheDir  = s.value("kernelCacheDir").toString();
    cuda.enableProfiling = s.value("enableProfiling", false).toBool();
    cuda.useGPU          = s.value("useGPU", true).toBool();
    cuda.blockSize       = s.value("blockSize", 256).toInt();
    s.endGroup();
}

void AppSettings::loadML(QSettings& s) {
    s.beginGroup("ML");
    ml.modelPath       = s.value("modelPath").toString();
    ml.quantLevel      = static_cast<MLSettings::QuantLevel>(
        s.value("quantLevel", static_cast<int>(MLSettings::QuantLevel::Q4_K_M)).toInt());
    ml.inferenceDevice = static_cast<MLSettings::InferenceDevice>(
        s.value("inferenceDevice", static_cast<int>(MLSettings::InferenceDevice::Auto)).toInt());
    ml.temperature   = s.value("temperature", 0.7).toDouble();
    ml.topP          = s.value("topP",         0.9).toDouble();
    ml.topK          = s.value("topK",          40).toInt();
    ml.maxNewTokens  = s.value("maxNewTokens", 512).toInt();
    ml.contextLength = s.value("contextLength",4096).toInt();
    ml.streamOutput  = s.value("streamOutput", true).toBool();
    s.endGroup();
}

void AppSettings::loadRecovery(QSettings& s) {
    s.beginGroup("Recovery");
    recovery.detectSTL          = s.value("detectSTL",          true).toBool();
    recovery.detectCrypto       = s.value("detectCrypto",       true).toBool();
    recovery.detectPatterns     = s.value("detectPatterns",     true).toBool();
    recovery.detectConcurrency  = s.value("detectConcurrency",  true).toBool();
    recovery.detectCuda         = s.value("detectCuda",         true).toBool();
    recovery.detectRTTI         = s.value("detectRTTI",         true).toBool();
    recovery.detectExceptions   = s.value("detectExceptions",   true).toBool();
    recovery.detectVirtual      = s.value("detectVirtual",      true).toBool();
    recovery.stlConfidence      = s.value("stlConf",    0.6).toDouble();
    recovery.cryptoConfidence   = s.value("cryptoConf", 0.7).toDouble();
    recovery.patternConfidence  = s.value("patternConf",0.5).toDouble();
    recovery.concurrencyConfidence = s.value("concurrConf",0.6).toDouble();
    s.endGroup();
}

void AppSettings::loadAdvanced(QSettings& s) {
    s.beginGroup("Advanced");
    advanced.verbosity = static_cast<AdvancedSettings::Verbosity>(
        s.value("verbosity", static_cast<int>(AdvancedSettings::Verbosity::Normal)).toInt());
    advanced.irDumpPath      = s.value("irDumpPath").toString();
    advanced.intermediateDir = s.value("intermediateDir").toString();
    advanced.dumpIR          = s.value("dumpIR",  false).toBool();
    advanced.dumpASM         = s.value("dumpASM", false).toBool();
    advanced.dumpCFG         = s.value("dumpCFG", false).toBool();
    advanced.dumpSSA         = s.value("dumpSSA", false).toBool();
    advanced.colorOutput     = s.value("colorOutput", true).toBool();
    advanced.maxFunctions    = s.value("maxFunctions", 0).toInt();
    advanced.demangleNames   = s.value("demangleNames", true).toBool();
    s.endGroup();
}

void AppSettings::loadPlugins(QSettings& s) {
    s.beginGroup("Plugins");
    plugins.searchPaths    = s.value("searchPaths").toStringList();
    plugins.enabledPlugins = s.value("enabledPlugins").toStringList();
    plugins.autoLoadPlugins = s.value("autoLoad", true).toBool();
    s.endGroup();
}

void AppSettings::loadDecompiler(QSettings& s) {
    s.beginGroup("Decompiler");
    decompiler.useCustomLlvmPasses = s.value("useCustomLlvmPasses", false).toBool();
    decompiler.llvmPassesDisabled  = s.value("llvmPassesDisabled").toStringList();
    decompiler.extraConfigPath     = s.value("extraConfigPath").toString();
    decompiler.decompileOutputDir  = s.value("decompileOutputDir").toString();
    decompiler.liveConsoleTail     = s.value("liveConsoleTail", false).toBool();
    decompiler.outputLang          = s.value("outputLang", QStringLiteral("c")).toString();
    decompiler.decompileProfile    = s.value("decompileProfile", QStringLiteral("balanced")).toString();
    s.endGroup();
}

// ─── Save ─────────────────────────────────────────────────────────────────────

void AppSettings::save() const {
    QSettings s = makeQSettings();
    saveGeneral (s);
    saveAnalysis(s);
    saveCUDA    (s);
    saveML      (s);
    saveRecovery(s);
    saveAdvanced(s);
    savePlugins   (s);
    saveDecompiler(s);
    s.sync();
}

void AppSettings::saveGeneral(QSettings& s) const {
    s.beginGroup("General");
    s.setValue("theme",          static_cast<int>(general.theme));
    s.setValue("font",           general.editorFont.toString());
    s.setValue("fontSize",       general.fontSize);
    s.setValue("language",       general.language);
    s.setValue("showLineNumbers",general.showLineNumbers);
    s.setValue("wordWrap",       general.wordWrap);
    s.setValue("restoreSession", general.restoreSession);
    s.setValue("lastOpenDir",    general.lastOpenDir);
    s.setValue("lastBinaryPath", general.lastBinaryPath);
    s.endGroup();
}

void AppSettings::saveAnalysis(QSettings& s) const {
    s.beginGroup("Analysis");
    s.setValue("enableTyping",       analysis.enableTyping);
    s.setValue("enablePatternMatch", analysis.enablePatternMatch);
    s.setValue("enableConcurrency",  analysis.enableConcurrency);
    s.setValue("enableCudaRecovery", analysis.enableCudaRecovery);
    s.setValue("enableSerialDetect", analysis.enableSerialDetect);
    s.setValue("enableModuleCluster",analysis.enableModuleCluster);
    s.setValue("enableCxxLifter",    analysis.enableCxxLifter);
    s.setValue("minTypeConf",        analysis.minTypeConfidence);
    s.setValue("minPatternConf",     analysis.minPatternConfidence);
    s.setValue("minRecovConf",       analysis.minRecoveryConfidence);
    s.setValue("maxTimeSecs",        analysis.maxAnalysisTimeSecs);
    s.setValue("threadCount",        analysis.threadCount);
    s.endGroup();
}

void AppSettings::saveCUDA(QSettings& s) const {
    s.beginGroup("CUDA");
    s.setValue("deviceName",      cuda.deviceName);
    s.setValue("deviceIndex",     cuda.deviceIndex);
    s.setValue("kernelCacheDir",  cuda.kernelCacheDir);
    s.setValue("enableProfiling", cuda.enableProfiling);
    s.setValue("useGPU",          cuda.useGPU);
    s.setValue("blockSize",       cuda.blockSize);
    s.endGroup();
}

void AppSettings::saveML(QSettings& s) const {
    s.beginGroup("ML");
    s.setValue("modelPath",       ml.modelPath);
    s.setValue("quantLevel",      static_cast<int>(ml.quantLevel));
    s.setValue("inferenceDevice", static_cast<int>(ml.inferenceDevice));
    s.setValue("temperature",     ml.temperature);
    s.setValue("topP",            ml.topP);
    s.setValue("topK",            ml.topK);
    s.setValue("maxNewTokens",    ml.maxNewTokens);
    s.setValue("contextLength",   ml.contextLength);
    s.setValue("streamOutput",    ml.streamOutput);
    s.endGroup();
}

void AppSettings::saveRecovery(QSettings& s) const {
    s.beginGroup("Recovery");
    s.setValue("detectSTL",         recovery.detectSTL);
    s.setValue("detectCrypto",      recovery.detectCrypto);
    s.setValue("detectPatterns",    recovery.detectPatterns);
    s.setValue("detectConcurrency", recovery.detectConcurrency);
    s.setValue("detectCuda",        recovery.detectCuda);
    s.setValue("detectRTTI",        recovery.detectRTTI);
    s.setValue("detectExceptions",  recovery.detectExceptions);
    s.setValue("detectVirtual",     recovery.detectVirtual);
    s.setValue("stlConf",     recovery.stlConfidence);
    s.setValue("cryptoConf",  recovery.cryptoConfidence);
    s.setValue("patternConf", recovery.patternConfidence);
    s.setValue("concurrConf", recovery.concurrencyConfidence);
    s.endGroup();
}

void AppSettings::saveAdvanced(QSettings& s) const {
    s.beginGroup("Advanced");
    s.setValue("verbosity",       static_cast<int>(advanced.verbosity));
    s.setValue("irDumpPath",      advanced.irDumpPath);
    s.setValue("intermediateDir", advanced.intermediateDir);
    s.setValue("dumpIR",          advanced.dumpIR);
    s.setValue("dumpASM",         advanced.dumpASM);
    s.setValue("dumpCFG",         advanced.dumpCFG);
    s.setValue("dumpSSA",         advanced.dumpSSA);
    s.setValue("colorOutput",     advanced.colorOutput);
    s.setValue("maxFunctions",    advanced.maxFunctions);
    s.setValue("demangleNames",   advanced.demangleNames);
    s.endGroup();
}

void AppSettings::savePlugins(QSettings& s) const {
    s.beginGroup("Plugins");
    s.setValue("searchPaths",    plugins.searchPaths);
    s.setValue("enabledPlugins", plugins.enabledPlugins);
    s.setValue("autoLoad",       plugins.autoLoadPlugins);
    s.endGroup();
}

void AppSettings::saveDecompiler(QSettings& s) const {
    s.beginGroup("Decompiler");
    s.setValue("useCustomLlvmPasses", decompiler.useCustomLlvmPasses);
    s.setValue("llvmPassesDisabled",  decompiler.llvmPassesDisabled);
    s.setValue("extraConfigPath",      decompiler.extraConfigPath);
    s.setValue("decompileOutputDir",   decompiler.decompileOutputDir);
    s.setValue("liveConsoleTail",      decompiler.liveConsoleTail);
    s.setValue("outputLang",           decompiler.outputLang);
    s.setValue("decompileProfile",     decompiler.decompileProfile);
    s.endGroup();
}

// ─── Reset ────────────────────────────────────────────────────────────────────

void AppSettings::resetToDefaults() {
    general    = {};
    analysis   = {};
    cuda       = {};
    ml         = {};
    recovery   = {};
    advanced   = {};
    plugins    = {};
    decompiler = {};
    emit settingsChanged();
}

void AppSettings::notifySettingsChanged() {
    emit settingsChanged();
}

// ─── Export / Import ─────────────────────────────────────────────────────────

bool AppSettings::exportToFile(const QString& path) const {
    QJsonObject root;

    QJsonObject gen;
    gen["theme"]        = static_cast<int>(general.theme);
    gen["font"]         = general.editorFont.toString();
    gen["fontSize"]     = general.fontSize;
    gen["language"]     = general.language;
    gen["showLineNums"] = general.showLineNumbers;
    gen["wordWrap"]     = general.wordWrap;
    root["General"]     = gen;

    QJsonObject ana;
    ana["enableTyping"]  = analysis.enableTyping;
    ana["threadCount"]   = analysis.threadCount;
    ana["maxTimeSecs"]   = analysis.maxAnalysisTimeSecs;
    root["Analysis"]     = ana;

    QJsonObject ml_obj;
    ml_obj["modelPath"]  = ml.modelPath;
    ml_obj["temperature"]= ml.temperature;
    ml_obj["topP"]       = ml.topP;
    ml_obj["topK"]       = ml.topK;
    root["ML"]           = ml_obj;

    QJsonObject dec;
    dec["useCustomLlvmPasses"] = decompiler.useCustomLlvmPasses;
    dec["llvmPassesDisabled"]  = QJsonArray::fromStringList(decompiler.llvmPassesDisabled);
    dec["extraConfigPath"]     = decompiler.extraConfigPath;
    dec["decompileOutputDir"]  = decompiler.decompileOutputDir;
    dec["liveConsoleTail"]     = decompiler.liveConsoleTail;
    dec["outputLang"]          = decompiler.outputLang;
    dec["decompileProfile"]    = decompiler.decompileProfile;
    root["Decompiler"]         = dec;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool AppSettings::importFromFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isNull() || !doc.isObject()) return false;

    auto root = doc.object();

    if (root.contains("General")) {
        auto gen = root["General"].toObject();
        general.theme      = static_cast<GeneralSettings::Theme>(gen.value("theme").toInt());
        QFont font; font.fromString(gen.value("font").toString());
        general.editorFont = font;
        general.fontSize   = gen.value("fontSize").toInt(10);
        general.language   = gen.value("language").toString("en");
        general.showLineNumbers = gen.value("showLineNums").toBool(true);
        general.wordWrap   = gen.value("wordWrap").toBool(false);
    }

    if (root.contains("Analysis")) {
        auto ana = root["Analysis"].toObject();
        analysis.enableTyping        = ana.value("enableTyping").toBool(true);
        analysis.threadCount         = ana.value("threadCount").toInt(0);
        analysis.maxAnalysisTimeSecs = ana.value("maxTimeSecs").toInt(300);
    }

    if (root.contains("ML")) {
        auto m = root["ML"].toObject();
        ml.modelPath   = m.value("modelPath").toString();
        ml.temperature = m.value("temperature").toDouble(0.7);
        ml.topP        = m.value("topP").toDouble(0.9);
        ml.topK        = m.value("topK").toInt(40);
    }

    if (root.contains("Decompiler")) {
        auto d = root["Decompiler"].toObject();
        decompiler.useCustomLlvmPasses = d.value("useCustomLlvmPasses").toBool(false);
        decompiler.extraConfigPath     = d.value("extraConfigPath").toString();
        decompiler.decompileOutputDir  = d.value("decompileOutputDir").toString();
        decompiler.liveConsoleTail     = d.value("liveConsoleTail").toBool(false);
        decompiler.outputLang          = d.value("outputLang").toString(QStringLiteral("c"));
        decompiler.decompileProfile    = d.value("decompileProfile").toString(QStringLiteral("balanced"));
        decompiler.llvmPassesDisabled.clear();
        const QJsonArray arr = d.value("llvmPassesDisabled").toArray();
        for (const QJsonValue& v : arr) {
            if (v.isString())
                decompiler.llvmPassesDisabled.append(v.toString());
        }
    }

    emit settingsChanged();
    return true;
}

} // namespace retdec::gui

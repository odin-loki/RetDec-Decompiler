/**
 * @file src/gui/project_file.cpp
 * @brief .retdec project JSON serialisation and deserialisation.
 */

#include "retdec/gui/project_file.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

namespace retdec {
namespace gui {

ProjectFile::ProjectFile(const QString& binaryPath)
    : binaryPath_(binaryPath) {}

// ─── I/O ─────────────────────────────────────────────────────────────────────

bool ProjectFile::load(const QString& projectPath) {
    QFile f(projectPath);
    if (!f.open(QIODevice::ReadOnly)) {
        lastError_ = "Cannot open project file: " + f.errorString();
        return false;
    }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        lastError_ = "JSON parse error: " + err.errorString();
        return false;
    }
    if (!doc.isObject()) {
        lastError_ = "Expected JSON object at top level";
        return false;
    }
    return fromJson(doc.object());
}

bool ProjectFile::save(const QString& projectPath) const {
    QFile f(projectPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lastError_ = "Cannot write project file: " + f.errorString();
        return false;
    }
    QJsonDocument doc(toJson());
    f.write(doc.toJson(QJsonDocument::Indented));
    modified_ = false;
    return true;
}

// ─── Stage tracking ───────────────────────────────────────────────────────────

void ProjectFile::setStageStatus(const QString& stage, const QString& status) {
    stages_[stage] = status;
    modified_ = true;
}

QString ProjectFile::stageStatus(const QString& stage) const {
    return stages_.value(stage, "pending");
}

QStringList ProjectFile::stageNames() const {
    return stages_.keys();
}

// ─── Annotations ─────────────────────────────────────────────────────────────

void ProjectFile::setAnnotation(uint64_t address, const Annotation& ann) {
    annotations_[address] = ann;
    modified_ = true;
}

std::optional<ProjectFile::Annotation>
ProjectFile::annotation(uint64_t address) const {
    auto it = annotations_.find(address);
    if (it == annotations_.end()) return std::nullopt;
    return it.value();
}

void ProjectFile::clearAnnotation(uint64_t address) {
    annotations_.remove(address);
    modified_ = true;
}

// ─── JSON ─────────────────────────────────────────────────────────────────────

QJsonObject ProjectFile::toJson() const {
    QJsonObject root;
    root["version"]  = kVersion;
    root["binary"]   = binaryPath_;
    root["arch"]     = arch_;
    root["os"]       = os_;
    root["entryPoint"] = static_cast<qint64>(entryPoint_);
    root["savedAt"]  = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["decompiledOutput"] = decompiledOutputPath_;
    if (!functionListFilter_.isEmpty())
        root["functionListFilter"] = functionListFilter_;
    if (callGraphDepth_ != 0)
        root["callGraphDepth"] = callGraphDepth_;

    // Stages.
    QJsonObject stagesObj;
    for (auto it = stages_.constBegin(); it != stages_.constEnd(); ++it)
        stagesObj[it.key()] = it.value();
    root["stages"] = stagesObj;

    // Annotations.
    QJsonObject annsObj;
    for (auto it = annotations_.constBegin(); it != annotations_.constEnd(); ++it) {
        QJsonObject a;
        a["name"]              = it.value().name;
        a["comment"]           = it.value().comment;
        a["signatureOverride"] = it.value().signatureOverride;
        annsObj[QString::number(it.key())] = a;
    }
    root["annotations"] = annsObj;

    return root;
}

bool ProjectFile::fromJson(const QJsonObject& obj) {
    int ver = obj["version"].toInt();
    if (ver != kVersion) {
        lastError_ = QString("Unsupported project version %1").arg(ver);
        return false;
    }
    binaryPath_           = obj["binary"].toString();
    arch_                 = obj["arch"].toString();
    os_                   = obj["os"].toString();
    entryPoint_           = static_cast<uint64_t>(obj["entryPoint"].toVariant().toLongLong());
    decompiledOutputPath_ = obj["decompiledOutput"].toString();
    functionListFilter_   = obj["functionListFilter"].toString();
    callGraphDepth_       = obj["callGraphDepth"].toInt(0);

    // Stages.
    stages_.clear();
    QJsonObject stagesObj = obj["stages"].toObject();
    for (auto it = stagesObj.constBegin(); it != stagesObj.constEnd(); ++it)
        stages_[it.key()] = it.value().toString();

    // Annotations.
    annotations_.clear();
    QJsonObject annsObj = obj["annotations"].toObject();
    for (auto it = annsObj.constBegin(); it != annsObj.constEnd(); ++it) {
        uint64_t addr = it.key().toULongLong();
        QJsonObject a = it.value().toObject();
        Annotation ann;
        ann.name              = a["name"].toString();
        ann.comment           = a["comment"].toString();
        ann.signatureOverride = a["signatureOverride"].toString();
        annotations_[addr] = ann;
    }

    modified_ = false;
    return true;
}

} // namespace gui
} // namespace retdec

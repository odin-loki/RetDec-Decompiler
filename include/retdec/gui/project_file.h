#ifndef RETDEC_GUI_PROJECT_FILE_H
#define RETDEC_GUI_PROJECT_FILE_H

#include <QString>
#include <QJsonObject>
#include <QStringList>
#include <optional>

namespace retdec {
namespace gui {

/**
 * @brief ProjectFile — .retdec JSON project serialisation.
 *
 * A retdec project stores:
 *   - path to the original binary
 *   - analysis settings (arch, OS, entry point override)
 *   - per-function user annotations (rename, comment, signature override)
 *   - analysis stage completion status
 *   - decompiled C output path (if saved)
 *
 * File format (JSON):
 * ```json
 * {
 *   "version": 1,
 *   "binary": "/path/to/file.elf",
 *   "arch": "x86_64",
 *   "os": "linux",
 *   "entryPoint": 0,
 *   "savedAt": "2026-03-26T12:00:00Z",
 *   "stages": {
 *     "ssa":           "done",
 *     "varRecovery":   "done",
 *     "typeInference": "pending"
 *   },
 *   "annotations": {
 *     "0x4010a0": { "name": "decrypt_payload", "comment": "RC4 decrypt" }
 *   },
 *   "decompiledOutput": "/path/to/output.c"
 * }
 * ```
 */
class ProjectFile {
public:
    ProjectFile() = default;
    explicit ProjectFile(const QString& binaryPath);

    // I/O
    bool load(const QString& projectPath);
    bool save(const QString& projectPath) const;

    // Accessors
    QString binaryPath()    const { return binaryPath_; }
    QString arch()          const { return arch_; }
    QString os()            const { return os_; }
    uint64_t entryPoint()   const { return entryPoint_; }
    QString decompiledPath()const { return decompiledOutputPath_; }

    // Mutators
    void setBinaryPath(const QString& p) {
        binaryPath_ = p;
        modified_   = true;
    }
    void setArch(const QString& a) {
        arch_     = a;
        modified_ = true;
    }
    void setOs(const QString& o) {
        os_       = o;
        modified_ = true;
    }
    void setEntryPoint(uint64_t ep) {
        entryPoint_ = ep;
        modified_   = true;
    }
    void setDecompiledPath(const QString& p) {
        decompiledOutputPath_ = p;
        modified_             = true;
    }

    // Stage tracking
    void setStageStatus(const QString& stage, const QString& status);
    QString stageStatus(const QString& stage) const;
    QStringList stageNames() const;

    // Annotations
    struct Annotation {
        QString name;
        QString comment;
        QString signatureOverride;
    };
    void setAnnotation(uint64_t address, const Annotation& ann);
    std::optional<Annotation> annotation(uint64_t address) const;
    void clearAnnotation(uint64_t address);

    bool isModified() const { return modified_; }
    void clearModified()    { modified_ = false; }
    QString lastError()     const { return lastError_; }

private:
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& obj);

    QString   binaryPath_;
    QString   arch_;
    QString   os_;
    uint64_t  entryPoint_ = 0;
    QString   decompiledOutputPath_;
    QMap<QString, QString>     stages_;
    QMap<uint64_t, Annotation> annotations_;
    mutable bool    modified_   = false;
    mutable QString lastError_;

    static constexpr int kVersion = 1;
};

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_PROJECT_FILE_H

#include "moc_reader.h"
#include "project_writer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <exception>

namespace {

void writeBytes(FILE* destination, const QByteArray& bytes)
{
    QFile stream;
    if (stream.open(destination, QIODevice::WriteOnly)) {
        stream.write(bytes);
        stream.flush();
    }
}

void writeJson(FILE* destination, const QJsonObject& object)
{
    writeBytes(destination, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject summary(const recovery::Model& model)
{
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("model"), model.displayName},
        {QStringLiteral("canExport"), model.unsupported.isEmpty()},
        {QStringLiteral("canvas"), QJsonObject{
            {QStringLiteral("width"), model.canvasWidth},
            {QStringLiteral("height"), model.canvasHeight},
            {QStringLiteral("pixelsPerUnit"), model.pixelsPerUnit}}},
        {QStringLiteral("counts"), QJsonObject{
            {QStringLiteral("parameters"), static_cast<qint64>(model.parameters.size())},
            {QStringLiteral("parts"), static_cast<qint64>(model.parts.size())},
            {QStringLiteral("deformers"), static_cast<qint64>(model.deformers.size())},
            {QStringLiteral("meshes"), static_cast<qint64>(model.meshes.size())},
            {QStringLiteral("textures"), static_cast<qint64>(model.textures.size())}}},
        {QStringLiteral("unsupported"), QJsonArray::fromStringList(model.unsupported)},
        {QStringLiteral("observations"), QJsonArray::fromStringList(model.observations)}
    };
}

QString exceptionMessage()
{
    try {
        throw;
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    } catch (const QString& error) {
        return error;
    } catch (...) {
        return QStringLiteral("发生未识别的错误。");
    }
}

}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Live2D Recovery"));
    const QStringList arguments = app.arguments();
    if (arguments.size() == 1 || arguments.contains(QStringLiteral("--help"))
        || arguments.contains(QStringLiteral("-h"))) {
        writeBytes(stdout, QStringLiteral(
            "Live2D Recovery\n\n"
            "用法：\n"
            "  recovery-cli --inspect <model.model3.json>\n"
            "  recovery-cli <model.model3.json> <output.cmo3>\n\n"
            "--inspect 输出模型概况及尚未支持的结构。\n"
            "恢复只写入新文件；目标存在或包含未支持结构时停止。\n").toUtf8());
        return 0;
    }

    const bool inspect = arguments.value(1) == QStringLiteral("--inspect");
    if (arguments.size() != 3 || (!inspect && arguments.value(1).startsWith(QLatin1Char('-')))) {
        writeJson(stderr, {{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), QStringLiteral("参数不正确，请使用 --help 查看用法。")} });
        return 2;
    }

    try {
        const QString input = arguments.at(inspect ? 2 : 1);
        QString output;
        if (!inspect) {
            output = QFileInfo(arguments.at(2)).absoluteFilePath();
            if (!output.endsWith(QStringLiteral(".cmo3"), Qt::CaseInsensitive)) {
                throw QStringLiteral("输出文件必须使用 .cmo3 扩展名。");
            }
            if (QFileInfo::exists(output)) {
                throw QStringLiteral("目标文件已经存在，拒绝覆盖。");
            }
            if (!QDir(QFileInfo(output).absolutePath()).exists()) {
                throw QStringLiteral("输出目录不存在。");
            }
        }

        const recovery::Model model = recovery::readModel(input);
        if (inspect) {
            writeJson(stdout, summary(model));
            return 0;
        }
        if (!model.unsupported.isEmpty()) {
            writeJson(stderr, {{QStringLiteral("ok"), false},
                               {QStringLiteral("error"), QStringLiteral("模型包含尚未支持的结构，未创建工程。")},
                               {QStringLiteral("unsupported"), QJsonArray::fromStringList(model.unsupported)}});
            return 3;
        }
        if (QFileInfo::exists(output)) {
            throw QStringLiteral("目标文件已经存在，拒绝覆盖。");
        }
        recovery::writeProject(model, output);
        if (!QFileInfo(output).isFile() || QFileInfo(output).size() == 0) {
            throw QStringLiteral("写入没有产生有效文件。");
        }
        writeJson(stdout, {{QStringLiteral("ok"), true},
                           {QStringLiteral("output"), QDir::toNativeSeparators(output)},
                           {QStringLiteral("message"), QStringLiteral("工程已保存，请在 Cubism Editor 中检查。")} });
        return 0;
    } catch (...) {
        writeJson(inspect ? stdout : stderr,
                  {{QStringLiteral("ok"), false}, {QStringLiteral("error"), exceptionMessage()}});
        return 1;
    }
}

#include "project_writer.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <cstdio>
#include <stdexcept>

// Entirely generated test data. No compiled models or editor libraries are needed.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 2 || app.arguments().size() > 3) return 2;
    const bool noncontiguous = app.arguments().size() == 3;
    if (noncontiguous && app.arguments()[2] != "--noncontiguous-parts") return 2;
    const QString output = QFileInfo(app.arguments()[1]).absoluteFilePath();
    try {
        const QString texture = QFileInfo(output).absolutePath() + "/fixture-texture.png";
        QImage image(32, 32, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, ((x / 4 + y / 4) % 2) ? QColor(40, 170, 140) : QColor(230, 100, 80));
        if (!image.save(texture)) throw std::runtime_error("Cannot save the test texture.");

        recovery::Model model;
        model.displayName = QStringLiteral("Synthetic · 模型 <测试>");
        model.canvasWidth = 400;
        model.canvasHeight = 400;
        model.pixelsPerUnit = 200;
        model.canvasOrigin = {200, 200};
        model.textures = {texture};
        model.inputFile = QStringLiteral("not-shipped-input.model3.json");

        recovery::Parameter move;
        move.id = "ParamMove"; move.name = "Move"; move.minimum = -1; move.maximum = 1;
        recovery::Parameter turn = move;
        turn.id = "ParamTurn"; turn.name = "Turn"; turn.minimum = 0;
        recovery::Parameter shape = move;
        shape.id = "ParamShape"; shape.name = "Shape"; shape.type = 1;
        model.parameters = {move, turn, shape};
        model.parts = {{"PartA", "Part A", -1}, {"PartB", "Part B", 0}};

        recovery::Deformer warp;
        warp.id = "WarpRoot"; warp.name = "Root warp"; warp.rows = 1; warp.columns = 1;
        recovery::Form warpForm;
        warpForm.points = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        warp.forms = {warpForm};
        recovery::Deformer rotation;
        rotation.id = "RotationFirst"; rotation.name = "First rotation";
        rotation.kind = recovery::Deformer::Kind::Rotation; rotation.parent = 0;
        recovery::Form rotationForm;
        rotationForm.scale = 1.0f / 200; rotationForm.origin = {0.5, 0.5};
        rotation.forms = {rotationForm};
        recovery::Deformer nestedWarp = warp;
        nestedWarp.id = "WarpNested"; nestedWarp.name = "Nested warp"; nestedWarp.parent = 1;
        recovery::Deformer nestedRotation = rotation;
        nestedRotation.id = "RotationNested"; nestedRotation.name = "Nested rotation";
        nestedRotation.parent = 2; nestedRotation.forms[0].scale = 1;
        model.deformers = {warp, rotation, nestedWarp, nestedRotation};

        recovery::Mesh mesh;
        mesh.id = "SyntheticMesh"; mesh.name = "Synthetic mesh"; mesh.part = 1;
        mesh.deformer = 0; mesh.flags = 4; mesh.triangles = {0, 1, 2};
        mesh.uvs = {{0, 0}, {1, 0}, {0, 1}};
        mesh.bindings = {{0, {-1, 0, 1}}, {1, {0, 1}}};
        for (int second = 0; second < 2; ++second) {
            for (int first = 0; first < 3; ++first) {
                recovery::Form form;
                form.points = {{0.2 + first * 0.02, 0.2 + second * 0.02}, {0.8, 0.2}, {0.2, 0.8}};
                form.drawOrder = 500;
                mesh.forms << form;
            }
        }
        recovery::BlendTrack blend;
        blend.parameter = 2; blend.keys = {-1, 0, 1}; blend.neutralKey = 1;
        for (int key = -1; key <= 1; ++key) {
            recovery::Form delta;
            delta.points = {{0, key * 0.05}, {0, 0}, {0, 0}};
            delta.opacity = 0; delta.scale = 0;
            delta.multiplyColor = {0, 0, 0}; delta.screenColor = {0, 0, 0};
            blend.deltas << delta;
        }
        mesh.blends = {blend};
        recovery::Mesh other = mesh;
        other.id = "SyntheticMeshB"; other.name = "Second mesh"; other.blends.clear();
        recovery::Mesh rootBefore = other;
        rootBefore.id = "RootBefore"; rootBefore.name = "Root before"; rootBefore.part = -1;
        recovery::Mesh partBefore = other;
        partBefore.id = "PartBefore"; partBefore.name = "Part before"; partBefore.part = 0;
        recovery::Mesh partAfter = partBefore;
        partAfter.id = "PartAfter"; partAfter.name = "Part after";
        recovery::Mesh rootAfter = rootBefore;
        rootAfter.id = "RootAfter"; rootAfter.name = "Root after";
        model.meshes = {mesh, other, rootBefore, partBefore, partAfter, rootAfter};
        // Both nested parts sit between two local meshes in their parent.
        // All draw orders match, so the part tree must retain this tie order.
        model.drawOrderSequence = {2, 3, 1, 0, 4, 5};
        if (noncontiguous) model.drawOrderSequence = {2, 3, 1, 4, 0, 5};
        recovery::Glue glue;
        glue.id = "SyntheticGlue"; glue.meshA = 0; glue.meshB = 1;
        glue.verticesA = {0}; glue.verticesB = {0}; glue.weightsA = {0.5}; glue.weightsB = {0.5}; glue.intensities = {1};
        model.glues = {glue};

        model.displayInfo = QJsonObject{
            {"Parameters", QJsonArray{
                QJsonObject{{"Id", "ParamMove"}, {"Name", "Move"}, {"GroupId", "Motion"}},
                QJsonObject{{"Id", "ParamTurn"}, {"Name", "Turn"}, {"GroupId", "Motion"}},
                QJsonObject{{"Id", "ParamShape"}, {"Name", "Shape"}, {"GroupId", "Motion"}}}},
            {"ParameterGroups", QJsonArray{QJsonObject{{"Id", "Motion"}, {"Name", "Motion"}, {"GroupId", ""}}}},
            {"CombinedParameters", QJsonArray{QJsonValue(QJsonArray{"ParamMove", "ParamTurn"})}}};
        model.effectGroups = QJsonArray{
            QJsonObject{{"Target", "Parameter"}, {"Name", "EyeBlink"}, {"Ids", QJsonArray{"ParamMove"}}},
            QJsonObject{{"Target", "Parameter"}, {"Name", "LipSync"}, {"Ids", QJsonArray{"ParamShape"}}}};

        if (noncontiguous) {
            bool refused = false;
            try { recovery::writeProject(model, output); }
            catch (const std::runtime_error& error) {
                const QString message = QString::fromUtf8(error.what());
                refused = message.contains("draw-order sequence") && message.contains("part hierarchy");
            }
            if (!refused || QFileInfo::exists(output))
                throw std::runtime_error("An unrepresentable part draw sequence was not safely refused.");
            std::puts("NONCONTIGUOUS_PARTS_REFUSED");
            return 0;
        }

        recovery::writeProject(model, output);
        QFile file(output);
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read the generated project.");
        const QByteArray before = file.readAll(); file.close();
        bool refused = false;
        try { recovery::writeProject(model, output); } catch (const std::runtime_error&) { refused = true; }
        if (!file.open(QIODevice::ReadOnly) || !refused || file.readAll() != before)
            throw std::runtime_error("Existing project protection failed.");
        std::puts("PROJECT_FIXTURE_OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

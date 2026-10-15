#pragma once

#include <QPointF>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>

namespace recovery {

struct Parameter {
    QString id;
    QString name;
    float minimum = 0;
    float maximum = 1;
    float initial = 0;
    int type = 0;
    bool repeat = false;
    int decimals = 3;
};

struct Binding {
    int parameter = -1;
    QVector<float> keys;
};

struct Form {
    QVector<QPointF> points;
    float opacity = 1;
    float drawOrder = 0;
    float angle = 0;
    float scale = 1;
    QPointF origin;
    bool reflectX = false;
    bool reflectY = false;
    std::array<float, 3> multiplyColor{1, 1, 1};
    std::array<float, 3> screenColor{0, 0, 0};
};

struct BlendConstraint {
    int parameter = -1;
    QVector<float> keys;
    QVector<float> weights;
};

struct BlendTrack {
    int parameter = -1;
    QVector<float> keys;
    int neutralKey = -1;
    // Every stored numeric channel is a delta, including opacity and scale.
    QVector<Form> deltas;
    QVector<int> constraints;
};

struct Glue {
    QString id;
    int meshA = -1;
    int meshB = -1;
    QVector<int> verticesA;
    QVector<int> verticesB;
    QVector<float> weightsA;
    QVector<float> weightsB;
    QVector<Binding> bindings;
    QVector<float> intensities;
};

struct Part {
    QString id;
    QString name;
    int parent = -1;
};

struct Deformer {
    enum class Kind { Warp, Rotation };
    QString id;
    QString name;
    int parent = -1;
    int part = -1;
    Kind kind = Kind::Warp;
    int rows = 0;
    int columns = 0;
    float baseAngle = 0;
    QVector<Binding> bindings;
    QVector<Form> forms;
    QVector<BlendTrack> blends;
};

struct Mesh {
    QString id;
    QString name;
    int part = -1;
    int deformer = -1;
    int texture = 0;
    int flags = 0;
    QVector<int> masks;
    QVector<int> triangles;
    QVector<QPointF> uvs;
    QVector<Binding> bindings;
    QVector<Form> forms;
    QVector<BlendTrack> blends;
};

struct Model {
    QString displayName;
    QString inputFile;
    QStringList textures;
    double canvasWidth = 0;
    double canvasHeight = 0;
    double pixelsPerUnit = 1;
    QPointF canvasOrigin;
    QVector<Parameter> parameters;
    QVector<Part> parts;
    QVector<Deformer> deformers;
    QVector<Mesh> meshes;
    QVector<Glue> glues;
    QVector<BlendConstraint> blendConstraints;
    QVector<int> drawOrderSequence;
    QJsonObject physics;
    QJsonObject displayInfo;
    QJsonArray effectGroups;
    QStringList unsupported;
    QStringList observations;
};

}

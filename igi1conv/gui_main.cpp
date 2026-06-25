#include "gui_main.h"

#include <QApplication>
#include <QMainWindow>
#include <QSplitter>
#include <QFileSystemModel>
#include <QTreeView>
#include <QCheckBox>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProcess>
#include <QFileInfo>
#include <QPixmap>
#include <QMessageBox>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTimer>
#include <QQuaternion>
#include "../source/parsers/iff_parser.h"
#include <QLineEdit>
#include <QGroupBox>
#include <QFontDatabase>
#include <QComboBox>
#include <QToolBar>
#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QCryptographicHash>
#include <QShortcut>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QElapsedTimer>

// Windows MCI (Media Control Interface) for in-process WAV playback.  We
// avoid Qt6::Multimedia so the GUI doesn't need an extra Qt module - the
// winmm API is always present on Windows and gives us play / pause /
// resume / seek / stop, which is exactly what a music-player toolbar
// needs.
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

enum LogLevel { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2, LOG_VERBOSE = 3 };

#include <functional>
static std::function<void(const QString&, LogLevel)> g_logger;

#include <QCloseEvent>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLTexture>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QToolTip>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLFunctions_3_2_Core>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QOpenGLVersionFunctionsFactory>
#endif
#include <QJsonArray>
#include <QJsonValue>
#include <QCoreApplication>
#include <QSortFilterProxyModel>
#include <QInputDialog>
#include <QClipboard>
#include <QStyleFactory>
#include <QTextDocument>
#include <QPainter>
#include <QScrollArea>
#include <QSpinBox>
#include <QListWidget>
#include <QColorDialog>
#include <QUuid>
#include "graph_parser.h"
#include <QProgressDialog>
#include "gif.h"

#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "tex_parser.h"
#include "cmd_olm.h"
#include "lightmap_resolver.h"
#include "mef_parser.h"
#include "mef_native.h"
#include "qsc_object_parser.h"

#include "../../third_party/tinygltf/stb_image.h"

static QImage loadImageSafe(const QString& path) {
    QImage img(path);
    if (img.isNull()) {
        int w, h, channels;
        unsigned char* data = stbi_load(path.toLocal8Bit().constData(), &w, &h, &channels, 4);
        if (data) {
            // Must copy because stbi_image_free will deallocate the memory
            img = QImage(data, w, h, w * 4, QImage::Format_RGBA8888).copy();
            stbi_image_free(data);
        }
    }
    return img;
}

// V-flip is a pure EXPORT-time transformation: see MefVToObjV in
// source/parsers/mef_exporter.cpp and the MefExportVFlip_*
// regression tests in tests/test_igi1conv_commands.cpp.  The 3D
// viewer is contractually required to render V as-is from the
// MEF (no flip), and that contract is pinned by the
// MefViewerDoesNotFlipV test.  No viewer-side V-flip helper
// exists on purpose.

static QString formatJson(const QJsonObject& obj, int indent = 0) {
    QString result;
    QString ind(indent, ' ');
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key().toLower() == "graph") continue;
        if (it.value().isObject()) {
            result += ind + it.key() + ":\n" + formatJson(it.value().toObject(), indent + 2);
        } else if (it.value().isArray()) {
            result += ind + it.key() + ": [Array]\n";
        } else {
            result += ind + it.key() + ": " + it.value().toVariant().toString() + "\n";
        }
    }
    return result;
}

class ModelViewer : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    struct SubMesh {
        int startIndex = 0;
        int count = 0;
        QString materialName;
        std::shared_ptr<QOpenGLTexture> texture;
        std::shared_ptr<QOpenGLTexture> lightmapTexture; // set by applyLightmapTextures()
        unsigned int drawMode = 0x0004; // GL_TRIANGLES
        bool useOverrideColor = false;
        bool disableDepthTest = false; // render on top (bones overlay)
        QVector4D overrideColor = QVector4D(1, 1, 1, 1);
    };

    void updateIffSkeleton() {
        if (!isIffAnimation || !currentIff.valid || currentIff.clips.empty()) return;

        const auto& clip  = currentIff.clips[currentClipIndex];
        const auto& skel  = currentIff.skeleton;

        // ── 1. Compute animated global bone transforms ────────────────────
        const float IGI_SCALE = 40.96f;
        animBoneTransforms.assign(skel.bone_count, QMatrix4x4());

        for (int i = 0; i < skel.bone_count; ++i) {
            QMatrix4x4 localMat;
            localMat.setToIdentity();

            QVector3D trans(0, 0, 0);
            if (i * 3 + 2 < (int)skel.translations.size())
                trans = QVector3D(skel.translations[i*3],
                                  skel.translations[i*3+1],
                                  skel.translations[i*3+2]);

            // Root bone: apply animated translation if available
            if (i == 0 && !clip.root_translations.empty()) {
                IffTranslationKey k1, k2;
                float t = 0.0f;
                if (animTime <= clip.root_translations.front().time) {
                    k1 = k2 = clip.root_translations.front();
                } else if (animTime >= clip.root_translations.back().time) {
                    k1 = k2 = clip.root_translations.back();
                } else {
                    for (size_t j = 0; j+1 < clip.root_translations.size(); j++) {
                        if (animTime >= clip.root_translations[j].time &&
                            animTime <= clip.root_translations[j+1].time) {
                            k1 = clip.root_translations[j];
                            k2 = clip.root_translations[j+1];
                            float dt = k2.time - k1.time;
                            t = (dt == 0) ? 0.0f : (animTime - k1.time) / dt;
                            break;
                        }
                    }
                }
                trans.setX(k1.pos[0] + (k2.pos[0] - k1.pos[0]) * t);
                trans.setY(k1.pos[1] + (k2.pos[1] - k1.pos[1]) * t);
                trans.setZ(k1.pos[2] + (k2.pos[2] - k1.pos[2]) * t);
            }

            // Animated rotation
            QQuaternion rot;
            rot.setScalar(1.0f);
            if (i < (int)clip.bone_rotations.size() && !clip.bone_rotations[i].empty()) {
                const auto& track = clip.bone_rotations[i];
                IffRotationKey k1, k2;
                float t = 0.0f;
                if (animTime <= track.front().time) {
                    k1 = k2 = track.front();
                } else if (animTime >= track.back().time) {
                    k1 = k2 = track.back();
                } else {
                    for (size_t j = 0; j+1 < track.size(); j++) {
                        if (animTime >= track[j].time && animTime <= track[j+1].time) {
                            k1 = track[j]; k2 = track[j+1];
                            float dt = k2.time - k1.time;
                            t = (dt == 0) ? 0.0f : (animTime - k1.time) / dt;
                            break;
                        }
                    }
                }
                QQuaternion q1(k1.rot[3], k1.rot[0], k1.rot[1], k1.rot[2]);
                QQuaternion q2(k2.rot[3], k2.rot[0], k2.rot[1], k2.rot[2]);
                rot = QQuaternion::slerp(q1, q2, t);
            }

            localMat.translate(trans / IGI_SCALE);
            localMat.rotate(rot);

            int parent = (i < (int)skel.parents.size()) ? (int)skel.parents[i] : -1;
            if (parent >= 0 && parent < i)
                animBoneTransforms[i] = animBoneTransforms[parent] * localMat;
            else
                animBoneTransforms[i] = localMat;
        }

        // ── 2. If we have a rest mesh (MEF bone model loaded), deform it ─
        if (hasRestMesh && !restMesh.empty() && !mefBoneWorldPos.empty()) {
            vertices.clear(); normals.clear();
            // uvs are already set from the MEF load and don't change
            // submeshes are already set from the MEF load

            const float IGI_SCALE = 40.96f;
            // MEF and IFF skeletons share the same bone local structure but
            // the IFF places the root at (0,0,0) while the MEF places it at
            // mefBoneWorldPos[0].  Add this offset after animating so the
            // deformed model stays in the MEF viewer's coordinate frame.
            QVector3D rootOffset(
                mefBoneWorldPos[0].x / IGI_SCALE,
                mefBoneWorldPos[0].y / IGI_SCALE,
                mefBoneWorldPos[0].z / IGI_SCALE
            );
            const auto& transforms = showRestPose ? iffRestBoneMats : animBoneTransforms;

            for (const auto& rv : restMesh) {
                uint16_t b = rv.boneIndex;
                if (b >= mefBoneWorldPos.size() || b >= transforms.size()) {
                    // Fallback: no deformation
                    vertices.push_back(rv.pos[0]); vertices.push_back(rv.pos[1]); vertices.push_back(rv.pos[2]);
                    normals.push_back(rv.normal[0]); normals.push_back(rv.normal[1]); normals.push_back(rv.normal[2]);
                    continue;
                }
                // MEF vertices are baked in MEF world space:
                //   rv.pos = (localVert + mefBoneWorldPos[b]) / IGI_SCALE
                // The local offset is identical in MEF and IFF bone-local space
                // because the two skeletons only differ by the root translation.
                glm::vec3 mefBwp = mefBoneWorldPos[b];
                QVector3D localOffset(
                    rv.pos[0] - mefBwp.x / IGI_SCALE,
                    rv.pos[1] - mefBwp.y / IGI_SCALE,
                    rv.pos[2] - mefBwp.z / IGI_SCALE
                );

                // Apply the bone transform in IFF space, then shift back to MEF space.
                QVector3D animatedPos = transforms[b].map(localOffset) + rootOffset;

                // Deform normal the same way (rotation only, no translation).
                QVector3D animNormal = transforms[b].mapVector(QVector3D(rv.normal[0], rv.normal[1], rv.normal[2]));

                vertices.push_back(animatedPos.x()); vertices.push_back(animatedPos.y()); vertices.push_back(animatedPos.z());
                normals.push_back(animNormal.x()); normals.push_back(animNormal.y()); normals.push_back(animNormal.z());
            }

            // Optionally add bone dots/lines as overlay
            if (showBonesOverlay) {
                addBonesOverlay(transforms, skel, rootOffset);
            }

            // Re-normalize deformed vertices using the SAME center/scale
            // that centerModel() computed during the original MEF load.
            // The rest mesh stores pre-normalized positions (IGI/40.96
            // units), so the deformed vertices are also in those units.
            // Without this step the model appears huge because the
            // viewer's camera transform expects normalized [-1,1] coords.
            for (int i = 0; i < (int)vertices.size(); i += 3) {
                vertices[i]   = (vertices[i]   - modelCx) / modelScale;
                vertices[i+1] = (vertices[i+1] - modelCy) / modelScale;
                vertices[i+2] = (vertices[i+2] - modelCz) / modelScale;
            }

            iffBuffersDirty = true;
            setupBuffers();
            return;
        }

        // ── 3. No rest mesh: show skeleton dots/lines (old behavior) ─────
        vertices.clear(); uvs.clear(); normals.clear();
        submeshes.clear();

        QVector<QVector3D> bonePos(skel.bone_count);
        for (int i = 0; i < skel.bone_count; ++i)
            bonePos[i] = animBoneTransforms[i].map(QVector3D(0,0,0));

        // Auto-fit camera
        QVector3D bbMin = bonePos.isEmpty() ? QVector3D(-1,-1,-1) : bonePos[0];
        QVector3D bbMax = bonePos.isEmpty() ? QVector3D( 1, 1, 1) : bonePos[0];
        for (const auto& p : bonePos) {
            bbMin.setX(std::min(bbMin.x(), p.x()));
            bbMin.setY(std::min(bbMin.y(), p.y()));
            bbMin.setZ(std::min(bbMin.z(), p.z()));
            bbMax.setX(std::max(bbMax.x(), p.x()));
            bbMax.setY(std::max(bbMax.y(), p.y()));
            bbMax.setZ(std::max(bbMax.z(), p.z()));
        }
        QVector3D center = (bbMin + bbMax) * 0.5f;
        float extent = std::max({(bbMax - bbMin).x(),
                                 (bbMax - bbMin).y(),
                                 (bbMax - bbMin).z(), 0.01f});
        modelCx = center.x(); modelCy = center.y(); modelCz = center.z();
        modelScale = extent;

        auto addV = [&](const QVector3D& p, const QVector3D& n) {
            float nx = (p.x() - center.x()) / extent;
            float ny = (p.y() - center.y()) / extent;
            float nz = (p.z() - center.z()) / extent;
            vertices.push_back(nx); vertices.push_back(ny); vertices.push_back(nz);
            uvs.push_back(0); uvs.push_back(0);
            normals.push_back(n.x()); normals.push_back(n.y()); normals.push_back(n.z());
        };

        // Bones as LINES
        int boneLineStart = 0;
        int boneLineCount = 0;
        for (int i = 0; i < skel.bone_count; ++i) {
            int parent = (i < (int)skel.parents.size()) ? (int)skel.parents[i] : -1;
            if (parent >= 0 && parent < i) {
                addV(bonePos[parent], QVector3D(0,1,0));
                addV(bonePos[i],      QVector3D(0,1,0));
                boneLineCount += 2;
            }
        }
        if (boneLineCount > 0) {
            SubMesh sm;
            sm.startIndex = boneLineStart;
            sm.count = boneLineCount;
            sm.drawMode = 0x0001; // GL_LINES
            sm.useOverrideColor = true;
            sm.disableDepthTest = true;
            sm.overrideColor = QVector4D(1.0f, 0.55f, 0.0f, 1.0f);
            submeshes.push_back(sm);
        }

        // Joint DOTS as tiny cubes
        int jointStart = vertices.size() / 3;
        float r = 0.03f;
        for (int i = 0; i < skel.bone_count; ++i) {
            float cx = (bonePos[i].x()-center.x())/extent;
            float cy = (bonePos[i].y()-center.y())/extent;
            float cz = (bonePos[i].z()-center.z())/extent;
            QVector3D o(cx, cy, cz);
            QVector3D verts[8] = {
                o+QVector3D(-r,-r,-r), o+QVector3D(r,-r,-r),
                o+QVector3D(r, r,-r), o+QVector3D(-r, r,-r),
                o+QVector3D(-r,-r, r), o+QVector3D(r,-r, r),
                o+QVector3D(r, r, r), o+QVector3D(-r, r, r)
            };
            int fi[36] = {0,1,2,0,2,3, 4,6,5,4,7,6, 0,4,5,0,5,1,
                          1,5,6,1,6,2, 2,6,7,2,7,3, 0,3,7,0,7,4};
            QVector3D fn[6] = {
                {0,0,-1},{0,0,1},{0,-1,0},{1,0,0},{0,1,0},{-1,0,0}};
            for (int f = 0; f < 6; f++) {
                for (int t = 0; t < 6; t++) {
                    int vi = fi[f*6+t];
                    QVector3D p = verts[vi];
                    vertices.push_back(p.x()); vertices.push_back(p.y()); vertices.push_back(p.z());
                    uvs.push_back(0); uvs.push_back(0);
                    normals.push_back(fn[f].x()); normals.push_back(fn[f].y()); normals.push_back(fn[f].z());
                }
            }
        }
        if (skel.bone_count > 0) {
            SubMesh jsm;
            jsm.startIndex = jointStart;
            jsm.count = skel.bone_count * 36;
            jsm.drawMode = 0x0004;
            jsm.useOverrideColor = true;
            jsm.disableDepthTest = true;
            jsm.overrideColor = QVector4D(0.3f, 0.9f, 1.0f, 1.0f);
            submeshes.push_back(jsm);
        }

        // XYZ axis cross at root
        {
            float ax = (bonePos[0].x()-center.x())/extent;
            float ay = (bonePos[0].y()-center.y())/extent;
            float az = (bonePos[0].z()-center.z())/extent;
            float al = 0.08f;
            int axStart = vertices.size() / 3;
            vertices<<ax<<ay<<az; uvs<<0<<0; normals<<1<<0<<0;
            vertices<<ax+al<<ay<<az; uvs<<0<<0; normals<<1<<0<<0;
            vertices<<ax<<ay<<az; uvs<<0<<0; normals<<0<<1<<0;
            vertices<<ax<<ay+al<<az; uvs<<0<<0; normals<<0<<1<<0;
            vertices<<ax<<ay<<az; uvs<<0<<0; normals<<0<<0<<1;
            vertices<<ax<<ay<<az+al; uvs<<0<<0; normals<<0<<0<<1;
            SubMesh xsm;
            xsm.startIndex = axStart;
            xsm.count = 6;
            xsm.drawMode = 0x0001;
            xsm.useOverrideColor = true;
            xsm.disableDepthTest = true;
            xsm.overrideColor = QVector4D(1.0f, 1.0f, 0.2f, 1.0f);
            submeshes.push_back(xsm);
        }

        iffBuffersDirty = true;
    }

    // Add bone dots/lines as an overlay on top of the deformed MEF mesh.
    // Called when showBonesOverlay is true and hasRestMesh is true.
    // rootOffset shifts IFF-space bone positions back into the MEF viewer frame.
    // Bone vertices are pushed in RAW MEF-space coordinates; the normalisation
    // loop in updateIffSkeleton() converts them to the viewer's [-1,1] frame
    // using the same modelCx/modelScale as the mesh, so bones and mesh stay
    // perfectly aligned.
    void addBonesOverlay(const std::vector<QMatrix4x4>& transforms,
                         const IffSkeleton& skel,
                         const QVector3D& rootOffset) {
        QVector<QVector3D> bonePos(skel.bone_count);
        for (int i = 0; i < skel.bone_count; ++i)
            bonePos[i] = transforms[i].map(QVector3D(0,0,0)) + rootOffset;

        float ext = modelScale;
        // Joint radius expressed in normalised viewer space; multiplied by
        // modelScale to keep the on-screen size constant regardless of the
        // model's physical extent.
        float r = 0.025f * ext;

        // ── Bone lines ────────────────────────────────────────────────────
        int boneStart = vertices.size() / 3;
        int boneLineCount = 0;
        for (int i = 0; i < skel.bone_count; ++i) {
            int parent = (i < (int)skel.parents.size()) ? (int)skel.parents[i] : -1;
            if (parent >= 0 && parent < i) {
                for (int v = 0; v < 2; v++) {
                    QVector3D p = (v == 0) ? bonePos[parent] : bonePos[i];
                    vertices.push_back(p.x());
                    vertices.push_back(p.y());
                    vertices.push_back(p.z());
                    uvs.push_back(0); uvs.push_back(0);
                    normals.push_back(0); normals.push_back(1); normals.push_back(0);
                }
                boneLineCount += 2;
            }
        }
        if (boneLineCount > 0) {
            SubMesh sm;
            sm.startIndex = boneStart;
            sm.count = boneLineCount;
            sm.drawMode = 0x0001; // GL_LINES
            sm.useOverrideColor = true;
            sm.disableDepthTest = true;
            sm.overrideColor = QVector4D(1.0f, 0.55f, 0.0f, 0.9f);
            submeshes.push_back(sm);
        }

        // ── Joint dots (small cubes as GL_TRIANGLES) ──────────────────────
        int jointStart = vertices.size() / 3;
        for (int i = 0; i < skel.bone_count; ++i) {
            QVector3D o(bonePos[i].x(), bonePos[i].y(), bonePos[i].z());
            QVector3D corner[8] = {
                o+QVector3D(-r,-r,-r), o+QVector3D(r,-r,-r),
                o+QVector3D(r, r,-r),  o+QVector3D(-r, r,-r),
                o+QVector3D(-r,-r, r), o+QVector3D(r,-r, r),
                o+QVector3D(r, r, r),  o+QVector3D(-r, r, r)
            };
            int idx[36] = {0,1,2,0,2,3, 4,6,5,4,7,6, 0,4,5,0,5,1,
                           1,5,6,1,6,2, 2,6,7,2,7,3, 0,3,7,0,7,4};
            QVector3D fn[6] = {
                {0,0,-1},{0,0,1},{0,-1,0},{1,0,0},{0,1,0},{-1,0,0}};
            for (int f = 0; f < 6; ++f) {
                for (int t = 0; t < 6; ++t) {
                    int vi = idx[f*6+t];
                    QVector3D p = corner[vi];
                    vertices.push_back(p.x()); vertices.push_back(p.y()); vertices.push_back(p.z());
                    uvs.push_back(0); uvs.push_back(0);
                    normals.push_back(fn[f].x()); normals.push_back(fn[f].y()); normals.push_back(fn[f].z());
                }
            }
        }
        SubMesh jsm;
        jsm.startIndex = jointStart;
        jsm.count = skel.bone_count * 36;
        jsm.drawMode = 0x0004; // GL_TRIANGLES
        jsm.useOverrideColor = true;
        jsm.disableDepthTest = true;
        jsm.overrideColor = QVector4D(0.3f, 0.9f, 1.0f, 1.0f);
        submeshes.push_back(jsm);
    }

    QTextEdit* infoOverlay;
    QLabel* coordsOverlay;
    QLabel* statsOverlay;
    QString currentModelName;
    QString currentModelId;
    QString currentJsonInfo;
    std::vector<SubMesh> submeshes;
    std::map<QString, std::shared_ptr<QOpenGLTexture>> textureCache;
    bool showWireframe = false;
    bool showGrid = true;
    QString currentModelPath;
    QString cacheDir;

    GraphFile currentGraph;
    int selectedGraphNodeId = -1;
    bool showGraphNodes = true;
    bool showGraphLinks = true;
    float modelCx = 0, modelCy = 0, modelCz = 0, modelScale = 1;
    float graphMaxDim = 1000.0f;
    float graphNodeScale = 1.0f;
    
    // IFF Animation State
    bool isIffAnimation = false;
    bool iffBuffersDirty = false;
    IffFile currentIff;
    QTimer* animTimer = nullptr;
    float animTime = 0.0f;
    int currentClipIndex = 0;
    bool iffPlaying = false;
    int animationFps = 30;       // configurable playback FPS (1-120)

    // ── Skeletal skinning state ────────────────────────────────────────
    // When a MEF bone model (type1) is loaded and an IFF is loaded on
    // top, we deform the MEF mesh each frame using the IFF bone
    // transforms instead of replacing it with bone dots/lines.
    // `restMesh` holds the MEF vertices in rest pose (with bone
    // indices), `iffRestBonePos` holds the IFF's OWN rest-pose bone
    // world positions (computed from TLST), and `showBonesOverlay`
    // toggles the skeleton visualization on top of the deformed mesh.
    struct RestVertex {
        float pos[3];      // rest-pose position (same units as `vertices`)
        float normal[3];   // rest-pose normal
        float uv[2];       // UV (unchanged by animation)
        uint16_t boneIndex; // which bone this vertex belongs to
        float weight;       // skinning weight (usually 1.0)
    };
    std::vector<RestVertex> restMesh;          // MEF mesh in rest pose
    std::vector<SubMesh>    restSubmeshes;     // submesh info for the MEF
    std::vector<glm::vec3>  mefBoneWorldPos;   // MEF's own bone world positions (IGI units)
    std::vector<QMatrix4x4> iffRestBoneMats;   // IFF rest-pose bone world transforms (4x4)
    std::vector<QMatrix4x4> animBoneTransforms; // animated bone world transforms from IFF
    bool hasRestMesh = false;       // true when MEF bone model is loaded
    bool showBonesOverlay = false;  // toggle skeleton dots/lines (key 'B')
    bool showRestPose = false;      // toggle rest-pose skinning (key 'P')

    // Callback so MainWindow media bar can react to time changes
    std::function<void(float time, float duration, int clip, int clipCount)> onIffTimeChanged;

    // Callback so MainWindow can update the 3D-view graph toolbar
    // (Total Nodes / Total Links labels and the Nodes/Links toggles)
    // every time a new graph is loaded.
    std::function<void(int nodeCount, int linkCount)> onGraphLoaded;

    // ── Public animation control API ─────────────────────────────────────────
    void iffPlay() {
        if (!isIffAnimation || currentIff.clips.empty()) return;
        iffPlaying = true;
        animTimer->start(1000 / animationFps);
    }
    void iffPause() {
        iffPlaying = false;
        animTimer->stop();
    }
    void iffTogglePlayPause() {
        if (iffPlaying) iffPause(); else iffPlay();
    }
    void iffSeekTo(float t) {
        if (!isIffAnimation || currentIff.clips.empty()) return;
        const auto& clip = currentIff.clips[currentClipIndex];
        animTime = std::max(0.0f, std::min(t, clip.duration));
        updateIffSkeleton();
        update();
        if (onIffTimeChanged) onIffTimeChanged(animTime, clip.duration, currentClipIndex, (int)currentIff.clips.size());
    }
    void iffStepForward() {
        if (!isIffAnimation || currentIff.clips.empty()) return;
        const auto& clip = currentIff.clips[currentClipIndex];
        animTime = std::min(animTime + (clip.duration / 30.0f), clip.duration);
        updateIffSkeleton(); update();
        if (onIffTimeChanged) onIffTimeChanged(animTime, clip.duration, currentClipIndex, (int)currentIff.clips.size());
    }
    void iffStepBackward() {
        if (!isIffAnimation || currentIff.clips.empty()) return;
        const auto& clip = currentIff.clips[currentClipIndex];
        animTime = std::max(0.0f, animTime - (clip.duration / 30.0f));
        updateIffSkeleton(); update();
        if (onIffTimeChanged) onIffTimeChanged(animTime, clip.duration, currentClipIndex, (int)currentIff.clips.size());
    }
    void iffSetClip(int idx) {
        if (!isIffAnimation || idx < 0 || idx >= (int)currentIff.clips.size()) return;
        currentClipIndex = idx;
        animTime = 0.0f;
        updateIffSkeleton(); update();
        const auto& clip = currentIff.clips[currentClipIndex];
        if (onIffTimeChanged) onIffTimeChanged(animTime, clip.duration, currentClipIndex, (int)currentIff.clips.size());
    }
    float iffGetDuration() const {
        if (!isIffAnimation || currentIff.clips.empty()) return 1.0f;
        return currentIff.clips[currentClipIndex].duration;
    }
    float iffGetTime() const { return animTime; }
    int iffGetClipCount() const { return (int)currentIff.clips.size(); }
    int iffGetClipIndex() const { return currentClipIndex; }
    int iffGetClipAnimId(int idx) const {
        if (idx < 0 || idx >= (int)currentIff.clips.size()) return -1;
        return (int)currentIff.clips[idx].animation_id;
    }

    // ── Public IFF API for the Animation mode panel ──────────────────────────
    //
    // `loadIffForAnimation` is the entry point used when the user clicks
    // the green Play button in the new "Animation" toolbar.  It:
    //   1. Decompiles the IFF bone-hierarchy file to a temp directory,
    //      then loads it through the existing `loadModel` path so the
    //      MEF body is rendered with the 3D viewer machinery.
    //   2. Re-loads the IFF itself so the skeleton + animation clips are
    //      parsed and `isIffAnimation` is set to true.
    //   3. Seeks to the requested clip and starts the play timer.
    //
    // We can't take both a MEF path AND an IFF path through a single
    // `loadModel` call because `loadModel` only supports one file at a
    // time.  The MEF is loaded first (so the body is on screen), then
    // the IFF is loaded and replaces the MEF geometry with the
    // animated skeleton + bones; the body still shows because the
    // IFF viewer paints both the bones AND the underlying MEF mesh
    // (see `updateIffSkeleton` / `loadMefRecursive` paths).  The
    // `playClip` method below then drives the timer.
    void playClip(int clipIndex) {
        if (!isIffAnimation || currentIff.clips.empty()) return;
        int idx = clipIndex;
        if (idx < 0) idx = 0;
        if (idx >= (int)currentIff.clips.size()) idx = (int)currentIff.clips.size() - 1;
        currentClipIndex = idx;
        animTime = 0.0f;
        updateIffSkeleton();
        update();
        iffPlaying = true;
        if (animTimer) animTimer->start(1000 / animationFps); // ~30 FPS for smooth playback
        if (onIffTimeChanged)
            onIffTimeChanged(0.0f, currentIff.clips[idx].duration, idx, (int)currentIff.clips.size());
    }

    // Load a fresh IFF (called by Animation mode after switching
    // bone-hierarchy files or model).
    void loadIff(const QString& path) {
        if (animTimer) animTimer->stop();
        makeCurrent();
        isIffAnimation = false;
        iffBuffersDirty = false;
        // NOTE: Do NOT clear restMesh/restSubmeshes/textureCache here.
        // When a MEF is loaded first and an IFF is loaded on top, we
        // need the rest-pose mesh to deform it with the IFF bones.
        // Only clear the dynamic vertex/normal arrays (they'll be
        // rebuilt by updateIffSkeleton).  Keep uvs and submeshes from
        // the MEF load so textures still render.
        vertices.clear(); normals.clear();
        // Don't clear uvs or submeshes - they come from the MEF
        // and are reused during skinning.

        currentIff = IFF_Parse(path.toStdString(), [](int level, const std::string& msg) {
            if (g_logger) g_logger(QString::fromStdString(msg), static_cast<LogLevel>(level));
        });
        if (currentIff.valid) {
            isIffAnimation = true;
            currentClipIndex = 0;
            animTime = 0.0f;
            // Compute the IFF's OWN rest-pose bone world positions.
            // These are used as the reference for skinning so that
            // at rest pose the deformed position == original position.
            computeIffRestBonePos();
            updateIffSkeleton();
            iffPlaying = true;
            if (animTimer) animTimer->start(1000 / animationFps); // 30 FPS
            if (!currentIff.clips.empty() && onIffTimeChanged)
                onIffTimeChanged(0.0f, currentIff.clips[0].duration, 0, (int)currentIff.clips.size());
        } else {
            isIffAnimation = false;
        }
        // Only recenter if we don't have a rest mesh (otherwise the
        // MEF's centering should be kept).
        if (!hasRestMesh) centerModel();
        setupBuffers();
        update();
    }

    // Compute the IFF's OWN rest-pose bone world transforms from the
    // TLST chunk (no animation applied).  These are 4x4 matrices in
    // viewer units (IGI / 40.96).  Used for matrix-palette skinning:
    //   delta = animTransform * restTransform.inverted()
    //   animatedPos = delta * restVertPos
    // At rest pose delta == identity so animatedPos == restVertPos.
    void computeIffRestBonePos() {
        const auto& skel = currentIff.skeleton;
        iffRestBoneMats.assign(skel.bone_count, QMatrix4x4());
        const float IGI_SCALE = 40.96f;
        for (int i = 0; i < skel.bone_count; ++i) {
            QMatrix4x4 localMat;
            localMat.setToIdentity();
            QVector3D trans(0, 0, 0);
            if (i * 3 + 2 < (int)skel.translations.size())
                trans = QVector3D(skel.translations[i*3],
                                  skel.translations[i*3+1],
                                  skel.translations[i*3+2]);
            localMat.translate(trans / IGI_SCALE);
            int parent = (i < (int)skel.parents.size()) ? (int)skel.parents[i] : -1;
            if (parent >= 0 && parent < i)
                iffRestBoneMats[i] = iffRestBoneMats[parent] * localMat;
            else
                iffRestBoneMats[i] = localMat;
        }
    }

    QVector3D worldToNormalized(float x, float y, float z) {
        return QVector3D((x - modelCx)/modelScale, (y - modelCy)/modelScale, (z - modelCz)/modelScale);
    }

    void setAnimationFps(int fps) {
        fps = std::clamp(fps, 1, 120);
        if (animationFps == fps) return;
        animationFps = fps;
        if (animTimer && animTimer->isActive()) {
            animTimer->start(1000 / animationFps);
        }
    }
    
    ModelViewer(QWidget* parent = nullptr) : QOpenGLWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        
        animTimer = new QTimer(this);
        connect(animTimer, &QTimer::timeout, this, [this]() {
            if (isIffAnimation && currentIff.valid && !currentIff.clips.empty()) {
                const auto& clip = currentIff.clips[currentClipIndex];
                animTime += 1000.0f / animationFps; // advance one frame's worth of time
                if (animTime >= clip.duration) {
                    animTime = 0.0f;
                    currentClipIndex = (currentClipIndex + 1) % currentIff.clips.size();
                }
                updateIffSkeleton();
                update();
                if (onIffTimeChanged) onIffTimeChanged(animTime, currentIff.clips[currentClipIndex].duration, currentClipIndex, (int)currentIff.clips.size());
            }
        });
        
        infoOverlay = new QTextEdit(this);
        infoOverlay->setReadOnly(true);
        infoOverlay->setStyleSheet("background-color: rgba(20, 20, 20, 180); color: #FFF; font-family: Consolas; font-size: 11px; border: none; padding: 5px;");
        infoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        infoOverlay->hide();

        coordsOverlay = new QLabel(this);
        coordsOverlay->setStyleSheet("background-color: rgba(0,0,0,180); color:#AAFFCC; padding:4px 8px; font-family:'Consolas',monospace; font-size:10px; border:1px solid #444;");
        coordsOverlay->hide();
        
        statsOverlay = new QLabel(this);
        statsOverlay->setStyleSheet("background-color: rgba(0,0,0,160); color:#FFCC66; padding:4px 8px; font-family:'Consolas',monospace; font-size:10px; border:1px solid #444; border-radius:3px;");
        statsOverlay->hide();
        
        // Keyboard shortcuts for 3D viewer
        auto addKey = [this](Qt::Key k, auto fn) {
            QShortcut* sc = new QShortcut(k, this);
            sc->setContext(Qt::WidgetShortcut);
            connect(sc, &QShortcut::activated, fn);
        };
        addKey(Qt::Key_W, [this]() { showWireframe = !showWireframe; update(); });
        addKey(Qt::Key_G, [this]() { showGrid = !showGrid; update(); });
        addKey(Qt::Key_R, [this]() { rotX=rotY=rotZ=0; transX=transY=0; zoom=3.0f; update(); updateCoordsOverlay(); });
        addKey(Qt::Key_I, [this]() { infoOverlay->setVisible(!infoOverlay->isVisible()); });
        addKey(Qt::Key_B, [this]() { showBonesOverlay = !showBonesOverlay; updateIffSkeleton(); update(); });
        addKey(Qt::Key_P, [this]() {
            showRestPose = !showRestPose;
            updateIffSkeleton();
            update();
            if (g_logger)
                g_logger(QString("[INFO] Rest-pose skinning %1").arg(showRestPose ? "enabled" : "disabled"), LOG_INFO);
        });
    }
    
    ~ModelViewer() {
        makeCurrent();
        textureCache.clear();
        submeshes.clear();
        vbo.destroy();
        doneCurrent();
    }

    std::shared_ptr<QOpenGLTexture> loadCachedTexture(const QString& texStr, const QFileInfo& info) {
        if (texStr.isEmpty()) return nullptr;
        QString key = texStr;
        key.replace("\\", "/");
        if (textureCache.find(key) != textureCache.end()) return textureCache[key];

        QString texPath = info.absolutePath() + "/" + key;
        QImage img = loadImageSafe(texPath);
        if (img.isNull()) {
            texPath = info.absolutePath() + "/" + QFileInfo(key).fileName();
            img = loadImageSafe(texPath);
        }
        if (img.isNull()) {
            texPath = info.absolutePath() + "/" + QFileInfo(key).completeBaseName() + ".png";
            img = loadImageSafe(texPath);
        }
        if (img.isNull()) {
            texPath = info.absolutePath() + "/" + QFileInfo(key).completeBaseName() + ".tga";
            img = loadImageSafe(texPath);
        }
        if (!img.isNull()) {
            auto tex = std::make_shared<QOpenGLTexture>(img);
            tex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
            tex->setMagnificationFilter(QOpenGLTexture::Linear);
            textureCache[key] = tex;
            return tex;
        }
        return nullptr;
    }

    void updateCoordsOverlay() {
        if (!coordsOverlay->isVisible()) coordsOverlay->show();
        coordsOverlay->setText(QString(
            "Model: %1 (%2)\n"
            "Cam: X%3 Y%4 Z%5 | Rot: X%6 Y%7 Z%8\n"
            "[W]ire [G]rid [R]eset [I]nfo [B]ones [P]ose | LMB=Rotate RMB=Pan Wheel=Zoom")
            .arg(currentModelName).arg(currentModelId)
            .arg(transX, 0,'f',2).arg(transY, 0,'f',2).arg(-zoom, 0,'f',2)
            .arg(rotX, 0,'f',0).arg(rotY, 0,'f',0).arg(rotZ, 0,'f',0));
    }

    void loadMefRecursive(const QString& path, const QMatrix4x4& transform, int depth) {
        if (depth > 20) return;
        
        QFileInfo info(path);
        if (!info.exists()) return;
        
        ParsedGeometry geo;
        try {
            geo = ParseMefFile(path.toStdString());
        } catch(...) { return; }

        // Store the MEF's own bone world positions for skinning.
        // These are in IGI world units and match the bone indices
        // stored in each vertex's boneIndex field.
        if (geo.modelType == 1 && !geo.bones.empty() && mefBoneWorldPos.empty()) {
            mefBoneWorldPos = ComputeBoneWorldPositions(geo.bones);
        }
        
        std::vector<std::shared_ptr<QOpenGLTexture>> loadedMaterials;
        if (!geo.pmtlTextures.empty()) {
            for (const auto& tex : geo.pmtlTextures) {
                loadedMaterials.push_back(loadCachedTexture(QString::fromStdString(tex), info));
            }
        } else {
            int maxSlot = -1;
            for (const auto& b : geo.renderBlocks) if (b.materialSlot > maxSlot) maxSlot = b.materialSlot;
            for (int i = 0; i <= maxSlot; ++i) {
                QString matName = QString("mat_%1.tga").arg(i);
                auto tex = loadCachedTexture(matName, info);
                if (!tex) {
                    QString tempDir = cacheDir + "/bundle/" + info.completeBaseName() + "/";
                    QFileInfo tempInfo(tempDir + matName);
                    tex = loadCachedTexture(tempInfo.fileName(), tempInfo);
                }
                loadedMaterials.push_back(tex);
            }
        }

        bool hasRawPos = false;
        for (const auto& v : geo.vertices) {
            if (v.rawPos.x != 0 || v.rawPos.y != 0 || v.rawPos.z != 0) { hasRawPos = true; break; }
        }
        bool isBoneModel = (geo.renderLayout.find("type1") != std::string::npos);
        // MEF stores V in different conventions depending on model type.
        // The V-flip decision itself is delegated to guiMefVToObjV()
        // so the rule stays in one place.  See the comment on that
        // helper near the top of this file.
        // older isBoneModel check (renderLayout contains "type1") missed
        // Type 3 lightmap models, leaving them upside-down in the viewer.
        const bool flipV = (geo.modelType == 0);

        for (const auto& block : geo.renderBlocks) {
            SubMesh sm;
            sm.startIndex = vertices.size() / 3;
            int triCount = 0;
            for (size_t i = 0; i < block.triangleCount; ++i) {
                size_t tIdx = block.triangleStart + i;
                if (tIdx < geo.triangles.size()) {
                    const auto& tri = geo.triangles[tIdx];
                    auto addVert = [&](uint32_t idx) {
                        if (idx < geo.vertices.size()) {
                            const auto& v = geo.vertices[idx];
                            // Bone models (type1): use v.pos which has bone world offsets baked in.
                            // Other XTRV models: use rawPos (raw game units). Collision fallback: v.pos * 40.96f.
                            glm::vec3 p = (isBoneModel || !hasRawPos) ? (v.pos * 40.96f) : v.rawPos;
                            QVector4D tp = transform * QVector4D(p.x, p.y, p.z, 1.0f);
                            float vx = tp.x() / 40.96f, vy = tp.y() / 40.96f, vz = tp.z() / 40.96f;
                            vertices.push_back(vx); vertices.push_back(vy); vertices.push_back(vz);

                            QVector4D tn = transform * QVector4D(v.normal.x, v.normal.y, v.normal.z, 0.0f);
                            QVector3D norm(tn.x(), tn.y(), tn.z());
                            norm.normalize();
                            normals.push_back(norm.x()); normals.push_back(norm.y()); normals.push_back(norm.z());

                            // V-flip is an EXPORT-time transformation
                            // only (see MefVToObjV in
                            // source/parsers/mef_exporter.cpp and the
                            // MefExportVFlip_* tests in
                            // tests/test_igi1conv_commands.cpp).  In
                            // the live 3D viewer we pass V through
                            // unchanged so the model is never
                            // displayed upside down.  Do not route
                            // this through the guiMefVToObjV helper
                            // and do NOT call MefVToObjV - the GUI's
                            // contract is identity.  Pinned by the
                            // MefViewerDoesNotFlipV regression test.
                            uvs.push_back(v.uv.x);
                            uvs.push_back(v.uv.y);

                            // Store rest-pose vertex for skeletal skinning.
                            // Only bone models (type1) have meaningful
                            // boneIndex/weight; for rigid models we
                            // store boneIndex=0 so skinning is a no-op.
                            if (isBoneModel) {
                                RestVertex rv;
                                rv.pos[0] = vx; rv.pos[1] = vy; rv.pos[2] = vz;
                                rv.normal[0] = norm.x(); rv.normal[1] = norm.y(); rv.normal[2] = norm.z();
                                rv.uv[0] = v.uv.x; rv.uv[1] = v.uv.y;
                                rv.boneIndex = v.boneIndex;
                                rv.weight = v.weight;
                                restMesh.push_back(rv);
                            }
                        } else {
                            vertices.push_back(0); vertices.push_back(0); vertices.push_back(0);
                            normals.push_back(0); normals.push_back(0); normals.push_back(0);
                            uvs.push_back(0); uvs.push_back(0);
                        }
                    };
                    addVert(tri[0]); addVert(tri[1]); addVert(tri[2]);
                    triCount += 3;
                }
            }
            sm.count = triCount;
            if (block.materialSlot >= 0 && block.materialSlot < loadedMaterials.size()) {
                sm.texture = loadedMaterials[block.materialSlot];
            }
            if (sm.count > 0) {
                submeshes.push_back(sm);
                if (isBoneModel) {
                    restSubmeshes.push_back(sm);
                }
            }
        }

        // Mark that we have a rest mesh for skinning
        if (isBoneModel && !restMesh.empty()) {
            hasRestMesh = true;
        }

        for (const auto& atta : geo.mefAttachments) {
            QMatrix4x4 aMat(
                atta.r00, atta.r01, atta.r02, atta.px,
                atta.r03, atta.r04, atta.r05, atta.py,
                atta.r06, atta.r07, atta.r08, atta.pz,
                0.0f,     0.0f,     0.0f,     1.0f
            );
            QMatrix4x4 childTransform = transform * aMat;
            
            QString childName = QString::fromLocal8Bit(atta.name, strnlen(atta.name, 16));
            QDir dir = info.absoluteDir();
            QString path1 = dir.filePath(childName + ".mef");
            QString path2 = dir.filePath(childName + ".MEF");
            QString path3 = dir.filePath(childName + ".mex");
            QString path4 = dir.filePath(childName + ".MEX");
            
            if (QFileInfo::exists(path1)) loadMefRecursive(path1, childTransform, depth + 1);
            else if (QFileInfo::exists(path2)) loadMefRecursive(path2, childTransform, depth + 1);
            else if (QFileInfo::exists(path3)) loadMefRecursive(path3, childTransform, depth + 1);
            else if (QFileInfo::exists(path4)) loadMefRecursive(path4, childTransform, depth + 1);
        }
    }

    // Build a per-vertex lightmap UV2 buffer (separate from the main vbo
    // so the shared pos/uv/normal layout used by every other view mode is
    // never touched) and assign a decoded .olm texture to each matching
    // SubMesh.  Mirrors loadMefRecursive's root-mesh block/triangle
    // traversal exactly so vertex indices line up with the main vbo.
    // If olmPaths.size() doesn't match the number of render blocks that
    // produced a SubMesh, olmPaths[0] is applied to every matched block.
    void applyLightmapTextures(const QString& mefPath, const std::vector<std::string>& olmPaths) {
        hasLightmapUvs = false;
        if (olmPaths.empty() || vertices.isEmpty()) return;

        ParsedGeometry geo;
        try {
            geo = ParseMefFile(mefPath.toStdString());
        } catch (...) { return; }
        if (geo.modelType != 3) return;

        std::vector<std::shared_ptr<QOpenGLTexture>> lightmapTextures;
        for (const auto& olmPath : olmPaths) {
            OLMFile olm = ParseOlm(olmPath);
            if (!olm.valid || olm.pixels.empty() || olm.layer.pixel_width == 0 || olm.layer.pixel_height == 0) {
                lightmapTextures.push_back(nullptr);
                continue;
            }
            QImage img(olm.layer.pixel_width, olm.layer.pixel_height, QImage::Format_RGBA8888);
            for (uint16_t y = 0; y < olm.layer.pixel_height; ++y) {
                for (uint16_t x = 0; x < olm.layer.pixel_width; ++x) {
                    size_t i = size_t(y) * olm.layer.pixel_width + x;
                    const auto& p = olm.pixels[i];
                    img.setPixelColor(x, y, QColor(p.r, p.g, p.b, p.a));
                }
            }
            auto tex = std::make_shared<QOpenGLTexture>(img);
            tex->setMinificationFilter(QOpenGLTexture::Linear);
            tex->setMagnificationFilter(QOpenGLTexture::Linear);
            lightmapTextures.push_back(tex);
        }

        QVector<float> uv2Data;
        std::vector<size_t> matchedSubmeshIdxs;
        size_t submeshIdx = 0;
        for (const auto& block : geo.renderBlocks) {
            int triCount = 0;
            for (size_t i = 0; i < block.triangleCount; ++i) {
                size_t tIdx = block.triangleStart + i;
                if (tIdx >= geo.triangles.size()) continue;
                const auto& tri = geo.triangles[tIdx];
                auto addUv2 = [&](uint32_t idx) {
                    if (idx < geo.vertices.size()) {
                        uv2Data.push_back(geo.vertices[idx].uv2.x);
                        uv2Data.push_back(geo.vertices[idx].uv2.y);
                    } else {
                        uv2Data.push_back(0.0f);
                        uv2Data.push_back(0.0f);
                    }
                };
                addUv2(tri[0]); addUv2(tri[1]); addUv2(tri[2]);
                triCount += 3;
            }
            if (triCount > 0) {
                if (submeshIdx < submeshes.size()) matchedSubmeshIdxs.push_back(submeshIdx);
                ++submeshIdx;
            }
        }

        bool countsMatch = (matchedSubmeshIdxs.size() == lightmapTextures.size());
        for (size_t k = 0; k < matchedSubmeshIdxs.size(); ++k) {
            std::shared_ptr<QOpenGLTexture> tex;
            if (countsMatch) tex = lightmapTextures[k];
            else if (!lightmapTextures.empty()) tex = lightmapTextures.front();
            submeshes[matchedSubmeshIdxs[k]].lightmapTexture = tex;
        }

        // Pad with zero UV2 for any vertices beyond this mef's own (root)
        // vertex count - i.e. recursively-loaded attachments, which never
        // get a lightmap in this pass.
        int totalVerts = vertices.size() / 3;
        while (uv2Data.size() < totalVerts * 2) uv2Data.push_back(0.0f);
        if (uv2Data.size() > totalVerts * 2) uv2Data.resize(totalVerts * 2);

        if (!lightmapUvVbo.isCreated()) lightmapUvVbo.create();
        lightmapUvVbo.bind();
        lightmapUvVbo.allocate(uv2Data.data(), uv2Data.size() * sizeof(float));
        lightmapUvVbo.release();
        hasLightmapUvs = true;
        update();
    }

    void exportGif(const QString& filename) {
        if (!isIffAnimation || !currentIff.valid || currentIff.clips.empty()) return;

        float totalDuration = 0;
        for (const auto& c : currentIff.clips) totalDuration += c.duration;
        
        QProgressDialog progress("Exporting GIF...", "Cancel", 0, totalDuration, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.show();

        int w = this->width();
        int h = this->height();

        GifWriter writer;
        GifBegin(&writer, filename.toLocal8Bit().constData(), w, h, 2, 8, true);

        float oldTime = animTime;
        int oldClip = currentClipIndex;
        bool oldGrid = showGrid;
        bool oldWire = showWireframe;
        showGrid = false;
        showWireframe = false;

        animTime = 0.0f;
        currentClipIndex = 0;
        
        float currentProgress = 0.0f;
        for (int i = 0; i < currentIff.clips.size(); ++i) {
            const auto& clip = currentIff.clips[i];
            currentClipIndex = i;
            for (float t = 0.0f; t < clip.duration; t += 16.0f) {
                if (progress.wasCanceled()) break;
                animTime = t;
                updateIffSkeleton();
                repaint(); // force a synchronous paint
                QImage frame = grabFramebuffer();
                frame = frame.convertToFormat(QImage::Format_RGBA8888);
                GifWriteFrame(&writer, frame.constBits(), w, h, 2, 8, true);
                
                currentProgress += 16.0f;
                progress.setValue((int)currentProgress);
                QCoreApplication::processEvents();
            }
            if (progress.wasCanceled()) break;
        }
        
        GifEnd(&writer);
        
        animTime = oldTime;
        currentClipIndex = oldClip;
        showGrid = oldGrid;
        showWireframe = oldWire;
        updateIffSkeleton();
        update();
        QMessageBox::information(this, "Export complete", "GIF exported successfully to\n" + filename);
    }

    void loadModel(const QString& path) {
        if (animTimer) animTimer->stop();
        isIffAnimation = false;
        iffBuffersDirty = false;
        hasRestMesh = false;
        hasLightmapUvs = false;
        makeCurrent();
        vertices.clear(); uvs.clear(); normals.clear();
        submeshes.clear();
        textureCache.clear();
        restMesh.clear();
        restSubmeshes.clear();
        mefBoneWorldPos.clear();
        animBoneTransforms.clear();
        // Loading a new non-graph model must clear any previous graph state,
        // otherwise mouse/keyboard handlers still think a graph is active and
        // can regenerate graph geometry on top of the MEF/IFF/OBJ view.
        currentGraph = GraphFile();
        selectedGraphNodeId = -1;
        if (onGraphLoaded) onGraphLoaded(0, 0);
        currentModelPath = path;
        
        QFileInfo info(path);
        QString ext = info.suffix().toLower();
        
        updateModelInfo(info.completeBaseName());
        
        if (ext == "mef" || ext == "mex") {
            QMatrix4x4 identity;
            identity.setToIdentity();
            loadMefRecursive(path, identity, 0);
        } else if (ext == "dat") {
            loadGraphData(path);
        } else if (ext == "iff" || ext == "bff") {
            currentIff = IFF_Parse(path.toStdString(), [](int level, const std::string& msg) {
                if (g_logger) g_logger(QString::fromStdString(msg), static_cast<LogLevel>(level));
            });
            if (currentIff.valid) {
                isIffAnimation = true;
                currentClipIndex = 0;
                animTime = 0.0f;

                // Compute first frame immediately so camera has real bounds
                updateIffSkeleton();

                if (!currentIff.clips.empty()) {
                    iffPlaying = true;
                    animTimer->start(1000 / animationFps);
                    if (onIffTimeChanged) onIffTimeChanged(0.0f, currentIff.clips[0].duration, 0, (int)currentIff.clips.size());
                }
            }
        } else if (ext == "obj") {
            std::vector<QVector3D> temp_v;
            std::vector<QVector2D> temp_vt;
            std::vector<QVector3D> temp_vn;
            
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString mtlLib = "";
                while (!file.atEnd()) {
                    QString line = file.readLine().trimmed();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                    if (parts.isEmpty()) continue;
                    
                    if (parts[0] == "v" && parts.size() >= 4) {
                        temp_v.push_back(QVector3D(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat()));
                    } else if (parts[0] == "vt" && parts.size() >= 3) {
                        temp_vt.push_back(QVector2D(parts[1].toFloat(), parts[2].toFloat()));
                    } else if (parts[0] == "vn" && parts.size() >= 4) {
                        temp_vn.push_back(QVector3D(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat()));
                    } else if (parts[0] == "usemtl" && parts.size() >= 2) {
                        if (!submeshes.empty() && submeshes.back().count == 0) {
                            submeshes.back().materialName = parts[1];
                        } else {
                            SubMesh sm;
                            sm.startIndex = vertices.size() / 3;
                            sm.count = 0;
                            sm.materialName = parts[1];
                            submeshes.push_back(sm);
                        }
                    } else if (parts[0] == "f" && parts.size() >= 4) {
                        if (submeshes.empty()) {
                            SubMesh sm;
                            sm.startIndex = vertices.size() / 3;
                            sm.count = 0;
                            submeshes.push_back(sm);
                        }
                        int startSize = vertices.size() / 3;
                        for (int i = 2; i < parts.size() - 1; ++i) {
                            parseFaceVertex(parts[1], temp_v, temp_vt, temp_vn);
                            parseFaceVertex(parts[i], temp_v, temp_vt, temp_vn);
                            parseFaceVertex(parts[i+1], temp_v, temp_vt, temp_vn);
                        }
                        submeshes.back().count += (vertices.size() / 3) - startSize;
                    } else if (parts[0] == "mtllib" && parts.size() >= 2) {
                        mtlLib = parts[1];
                    }
                }
                
                if (!mtlLib.isEmpty()) {
                    QString mtlPath = info.absolutePath() + "/" + mtlLib;
                    QFile mfile(mtlPath);
                    if (mfile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        QString currentMaterial = "";
                        while (!mfile.atEnd()) {
                            QString mline = mfile.readLine().trimmed();
                            if (mline.startsWith("newmtl ")) {
                                currentMaterial = mline.mid(7).trimmed();
                            } else if (mline.startsWith("map_Kd ") && !currentMaterial.isEmpty()) {
                                QString texStr = mline.mid(7).trimmed();
                                auto tex = loadCachedTexture(texStr, info);
                                for (auto& sm : submeshes) {
                                    if (sm.materialName == currentMaterial) {
                                        sm.texture = tex;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        centerModel();
        setupBuffers();
        update();
    }

    // Public wrapper so external widgets (graph toolbar buttons) can
    // request a geometry rebuild when graphNodeScale / showGraphNodes
    // / showGraphLinks change.  Only rebuilds if a graph is loaded.
    void regenerateGraphGeometry() {
        if (!currentGraph.valid) return;
        generateGraphGeometry();
    }

protected:
    void parseFaceVertex(const QString& str, const std::vector<QVector3D>& tv, const std::vector<QVector2D>& tvt, const std::vector<QVector3D>& tvn) {
        QStringList p = str.split('/');
        int vIdx = p.size() > 0 && !p[0].isEmpty() ? p[0].toInt() - 1 : -1;
        int vtIdx = p.size() > 1 && !p[1].isEmpty() ? p[1].toInt() - 1 : -1;
        int vnIdx = p.size() > 2 && !p[2].isEmpty() ? p[2].toInt() - 1 : -1;
        
        if (vIdx >= 0 && vIdx < tv.size()) {
            vertices.push_back(tv[vIdx].x()); vertices.push_back(tv[vIdx].y()); vertices.push_back(tv[vIdx].z());
        } else {
            vertices.push_back(0); vertices.push_back(0); vertices.push_back(0);
        }
        
        if (vtIdx >= 0 && vtIdx < tvt.size()) {
            uvs.push_back(tvt[vtIdx].x()); uvs.push_back(tvt[vtIdx].y());
        } else {
            uvs.push_back(0); uvs.push_back(0);
        }
        
        if (vnIdx >= 0 && vnIdx < tvn.size()) {
            normals.push_back(tvn[vnIdx].x()); normals.push_back(tvn[vnIdx].y()); normals.push_back(tvn[vnIdx].z());
        } else {
            normals.push_back(0); normals.push_back(1); normals.push_back(0);
        }
    }

    void centerModel() {
        if (vertices.isEmpty()) return;
        float minX = vertices[0], maxX = minX;
        float minY = vertices[1], maxY = minY;
        float minZ = vertices[2], maxZ = minZ;
        for (int i=0; i<vertices.size(); i+=3) {
            minX = std::min(minX, vertices[i]); maxX = std::max(maxX, vertices[i]);
            minY = std::min(minY, vertices[i+1]); maxY = std::max(maxY, vertices[i+1]);
            minZ = std::min(minZ, vertices[i+2]); maxZ = std::max(maxZ, vertices[i+2]);
        }
        float cx = (minX + maxX)/2.f; float cy = (minY + maxY)/2.f; float cz = (minZ + maxZ)/2.f;
        float maxDim = std::max({maxX - minX, maxY - minY, maxZ - minZ});
        if (maxDim == 0) maxDim = 1.0f;
        modelCx = cx; modelCy = cy; modelCz = cz; modelScale = maxDim;
        for (int i=0; i<vertices.size(); i+=3) {
            vertices[i] = (vertices[i]-cx)/maxDim;
            vertices[i+1] = (vertices[i+1]-cy)/maxDim;
            vertices[i+2] = (vertices[i+2]-cz)/maxDim;
        }
    }

    void setupBuffers() {
        if (vertices.isEmpty()) return;
        QVector<float> bufferData;
        for (int i=0; i<vertices.size()/3; ++i) {
            bufferData.push_back(vertices[i*3]); bufferData.push_back(vertices[i*3+1]); bufferData.push_back(vertices[i*3+2]);
            bufferData.push_back(uvs[i*2]); bufferData.push_back(uvs[i*2+1]);
            bufferData.push_back(normals[i*3]); bufferData.push_back(normals[i*3+1]); bufferData.push_back(normals[i*3+2]);
        }
        if (!vbo.isCreated()) vbo.create();
        vbo.bind();
        vbo.allocate(bufferData.data(), bufferData.size() * sizeof(float));
        vbo.release();
    }

    void loadGraphData(const QString& path) {
        currentGraph = GRAPH_Parse(path.toStdString());
        if (!currentGraph.valid) return;
        statsOverlay->setText(QString("Graph loaded: %1 nodes, %2 edges")
                              .arg(currentGraph.nodes.size())
                              .arg(currentGraph.edges.size()));
        if (onGraphLoaded) {
            onGraphLoaded((int)currentGraph.nodes.size(),
                          (int)currentGraph.edges.size());
        }

        if (!currentGraph.nodes.empty()) {
            float minX = currentGraph.nodes[0].x, maxX = minX;
            float minY = currentGraph.nodes[0].y, maxY = minY;
            float minZ = currentGraph.nodes[0].z, maxZ = minZ;
            for (const auto& node : currentGraph.nodes) {
                minX = std::min(minX, (float)node.x); maxX = std::max(maxX, (float)node.x);
                minY = std::min(minY, (float)node.y); maxY = std::max(maxY, (float)node.y);
                minZ = std::min(minZ, (float)node.z); maxZ = std::max(maxZ, (float)node.z);
            }
            graphMaxDim = std::max({maxX - minX, maxY - minY, maxZ - minZ});
            if (graphMaxDim < 1.0f) graphMaxDim = 1000.0f;
        } else {
            graphMaxDim = 1000.0f;
        }

        generateGraphGeometry();
    }

    void generateGraphGeometry() {
        vertices.clear(); uvs.clear(); normals.clear(); submeshes.clear(); textureCache.clear();

        if (showGraphNodes) {
            for (const auto& node : currentGraph.nodes) {
                SubMesh sm;
                sm.startIndex = vertices.size() / 3;
                sm.drawMode = 0x0004; // GL_TRIANGLES
                sm.useOverrideColor = true;

                float r = 0.85f, g = 0.12f, b = 0.12f;
                if (node.id == selectedGraphNodeId) { r=1.0f; g=0.6f; b=0.0f; }
                else if (node.criteria.find("DOOR") != std::string::npos) { r=1.0f; g=1.0f; b=0.0f; }
                else if (node.criteria.find("STAIR") != std::string::npos) { r=1.0f; g=0.0f; b=1.0f; }
                else if (node.criteria.find("VIEW") != std::string::npos) { r=0.0f; g=1.0f; b=1.0f; }
                sm.overrideColor = QVector4D(r, g, b, 1.0f);

                float baseH = std::max(50.0f, graphMaxDim * 0.015f) * graphNodeScale;
                float H = baseH * std::max(1.0f, (float)node.radius); 
                QVector3D c(node.x, node.y, node.z);
                QVector3D p0(c.x()-H, c.y()-H, c.z());
                QVector3D p1(c.x()+H, c.y()-H, c.z());
                QVector3D p2(c.x()+H, c.y()+H, c.z());
                QVector3D p3(c.x()-H, c.y()+H, c.z());
                QVector3D p4(c.x()-H, c.y()-H, c.z() + 2*H);
                QVector3D p5(c.x()+H, c.y()-H, c.z() + 2*H);
                QVector3D p6(c.x()+H, c.y()+H, c.z() + 2*H);
                QVector3D p7(c.x()-H, c.y()+H, c.z() + 2*H);

                auto addQuad = [&](const QVector3D& v0, const QVector3D& v1, const QVector3D& v2, const QVector3D& v3, const QVector3D& n) {
                    vertices << v0.x() << v0.y() << v0.z(); normals << n.x() << n.y() << n.z(); uvs << 0 << 0;
                    vertices << v1.x() << v1.y() << v1.z(); normals << n.x() << n.y() << n.z(); uvs << 1 << 0;
                    vertices << v2.x() << v2.y() << v2.z(); normals << n.x() << n.y() << n.z(); uvs << 1 << 1;
                    vertices << v0.x() << v0.y() << v0.z(); normals << n.x() << n.y() << n.z(); uvs << 0 << 0;
                    vertices << v2.x() << v2.y() << v2.z(); normals << n.x() << n.y() << n.z(); uvs << 1 << 1;
                    vertices << v3.x() << v3.y() << v3.z(); normals << n.x() << n.y() << n.z(); uvs << 0 << 1;
                };

                addQuad(p4, p5, p6, p7, QVector3D(0, 0, 1)); // Top
                addQuad(p1, p0, p3, p2, QVector3D(0, 0, -1)); // Bottom
                addQuad(p0, p1, p5, p4, QVector3D(0, -1, 0)); // Front
                addQuad(p2, p3, p7, p6, QVector3D(0, 1, 0)); // Back
                addQuad(p3, p0, p4, p7, QVector3D(-1, 0, 0)); // Left
                addQuad(p1, p2, p6, p5, QVector3D(1, 0, 0)); // Right

                sm.count = 36;
                submeshes.push_back(sm);
            }
        }

        if (showGraphLinks) {
            SubMesh lineSm;
            lineSm.startIndex = vertices.size() / 3;
            lineSm.drawMode = 0x0001; // GL_LINES
            lineSm.useOverrideColor = true;
            lineSm.overrideColor = QVector4D(0.0f, 1.0f, 1.0f, 1.0f); // Cyan
            
            for (const auto& edge : currentGraph.edges) {
                const GraphNode* n1 = GRAPH_FindNode(currentGraph, edge.node1);
                const GraphNode* n2 = GRAPH_FindNode(currentGraph, edge.node2);
                if (n1 && n2) {
                    vertices << n1->x << n1->y << n1->z;
                    normals << 0 << 1 << 0; uvs << 0 << 0;
                    vertices << n2->x << n2->y << n2->z;
                    normals << 0 << 1 << 0; uvs << 0 << 0;
                    lineSm.count += 2;
                }
            }
            if (lineSm.count > 0) submeshes.push_back(lineSm);
        }
    }

    void updateModelInfo(const QString& baseName) {
        currentModelId = baseName;
        if (baseName.isEmpty()) {
            infoOverlay->hide();
            return;
        }

        static QJsonArray modelsArray;
        static QJsonObject allLevelsObj;
        static bool loaded = false;

        if (!loaded) {
            loaded = true;
            QString appDir = QCoreApplication::applicationDirPath();
            QFile f1(appDir + "/IGIModels.json");
            if (f1.open(QIODevice::ReadOnly)) {
                modelsArray = QJsonDocument::fromJson(f1.readAll()).array();
            }
            QFile f2(appDir + "/IGIModelsAllLevel.json");
            if (f2.open(QIODevice::ReadOnly)) {
                allLevelsObj = QJsonDocument::fromJson(f2.readAll()).object();
            }
        }

        // Find Model Name from IGIModels.json
        QString modelName = "Unknown";
        for (const QJsonValue& val : modelsArray) {
            QJsonObject obj = val.toObject();
            if (obj["ModelId"].toString().toLower() == baseName.toLower()) {
                modelName = obj["ModelName"].toString();
                break;
            }
        }
        currentModelName = modelName;

        QJsonObject firstInstance;
        bool found = false;
        
        for (auto it = allLevelsObj.begin(); it != allLevelsObj.end() && !found; ++it) {
            QJsonObject levelObj = it.value().toObject();
            for (auto catIt = levelObj.begin(); catIt != levelObj.end() && !found; ++catIt) {
                QJsonArray items = catIt.value().toArray();
                for (const QJsonValue& itemVal : items) {
                    QJsonObject item = itemVal.toObject();
                    bool match = false;
                    if (item.contains("Model ID") && item["Model ID"].toString().toLower() == baseName.toLower()) {
                        match = true;
                    } else if (item.contains("Model") && item["Model"].toObject()["ID"].toString().toLower() == baseName.toLower()) {
                        match = true;
                    }

                    if (match) {
                        firstInstance = item;
                        found = true;
                        break;
                    }
                }
            }
        }

        currentJsonInfo = "";
        if (found) {
            currentJsonInfo = formatJson(firstInstance);
        }

        infoOverlay->setPlainText(QString(
            "=== %1 (%2) ===\n"
            "%3\n"
            "--- DAT Properties ---\n%4")
            .arg(currentModelName).arg(currentModelId)
            .arg(currentModelPath.isEmpty() ? "" : "Path: " + currentModelPath)
            .arg(currentJsonInfo));
        infoOverlay->show();
        
        // Update stats overlay
        statsOverlay->show();
        statsOverlay->setText(QString("V:%1 T:%2 M:%3")
            .arg(vertices.size()/3)
            .arg(vertices.size()/9)
            .arg(submeshes.size()));
    }

    void initializeGL() override {
        initializeOpenGLFunctions();
        // clear color set per-frame in paintGL to match current theme
        glEnable(GL_DEPTH_TEST);

        QVector<float> gridLines;
        for (int i = -10; i <= 10; ++i) {
            gridLines << i << 0.0f << -10.0f << 0.0f<<0.0f << 0.0f<<1.0f<<0.0f;
            gridLines << i << 0.0f << 10.0f  << 0.0f<<0.0f << 0.0f<<1.0f<<0.0f;
            gridLines << -10.0f << 0.0f << i << 0.0f<<0.0f << 0.0f<<1.0f<<0.0f;
            gridLines << 10.0f  << 0.0f << i << 0.0f<<0.0f << 0.0f<<1.0f<<0.0f;
        }
        gridVertexCount = gridLines.size() / 8;
        gridVbo.create();
        gridVbo.bind();
        gridVbo.allocate(gridLines.data(), gridLines.size() * sizeof(float));
        gridVbo.release();

        const char* vsrc =
            "#version 330 core\n"
            "layout(location = 0) in vec3 aPos;\n"
            "layout(location = 1) in vec2 aTex;\n"
            "layout(location = 2) in vec3 aNorm;\n"
            "layout(location = 3) in vec2 aUV2;\n"
            "out vec2 TexCoord;\n"
            "out vec2 LightmapCoord;\n"
            "out vec3 FragPos;\n"
            "out vec3 Normal;\n"
            "uniform mat4 model;\n"
            "uniform mat4 view;\n"
            "uniform mat4 projection;\n"
            "void main() {\n"
            "    FragPos = vec3(view * model * vec4(aPos, 1.0));\n"
            "    Normal = mat3(transpose(inverse(view * model))) * aNorm;\n"
            "    TexCoord = aTex;\n"
            "    LightmapCoord = aUV2;\n"
            "    gl_Position = projection * vec4(FragPos, 1.0);\n"
            "}\n";

        const char* fsrc =
            "#version 330 core\n"
            "in vec2 TexCoord;\n"
            "in vec2 LightmapCoord;\n"
            "in vec3 FragPos;\n"
            "in vec3 Normal;\n"
            "out vec4 FragColor;\n"
            "uniform sampler2D texture1;\n"
            "uniform sampler2D lightmapTex;\n"
            "uniform bool hasTexture;\n"
            "uniform bool hasLightmap;\n"
            "uniform bool useOverrideColor;\n"
            "uniform vec4 overrideColor;\n"
            "void main() {\n"
            "    vec3 norm = normalize(Normal);\n"
            "    vec3 viewDir = vec3(0.0, 0.0, 1.0);\n" // Directional headlight
            "    float diff = max(abs(dot(norm, viewDir)), 0.1);\n" // Two-sided lighting
            "    float ambient = 0.3;\n"
            "    float lighting = min(diff + ambient, 1.0);\n"
            "    vec4 baseColor = vec4(0.8, 0.8, 0.8, 1.0);\n"
            "    if (hasTexture) baseColor = texture(texture1, TexCoord);\n"
            "    if (hasLightmap) baseColor.rgb *= texture(lightmapTex, LightmapCoord).rgb;\n"
            "    if (useOverrideColor) baseColor = overrideColor;\n"
            "    FragColor = vec4(lighting * baseColor.rgb, baseColor.a);\n"
            "}\n";

        program.addShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
        program.addShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
        program.link();
    }

    void resizeGL(int w, int h) override {
        glViewport(0, 0, w, h);
    }

    void paintGL() override {
        QColor bg = QApplication::palette().color(QPalette::Window);
        glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (vertices.isEmpty()) return;
        
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_2_Core>(QOpenGLContext::currentContext());
#else
        auto* gl = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_3_2_Core>();
#endif
        if (gl) {
            if (showWireframe) gl->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            else gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        program.bind();
        QMatrix4x4 projection, view, model;
        projection.perspective(45.0f, float(width()) / float(height() ? height() : 1), 0.1f, 100.0f);
        view.translate(transX, transY, -zoom);
        
        if (showGrid && gridVertexCount > 0) {
            gridVbo.bind();
            program.enableAttributeArray(0);
            program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 8 * sizeof(float));
            program.enableAttributeArray(1);
            program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 2, 8 * sizeof(float));
            program.enableAttributeArray(2);
            program.setAttributeBuffer(2, GL_FLOAT, 5 * sizeof(float), 3, 8 * sizeof(float));
            program.setUniformValue("projection", projection);
            program.setUniformValue("view", view);
            QMatrix4x4 gridModel;
            program.setUniformValue("model", gridModel);
            program.setUniformValue("hasTexture", false);
            program.setUniformValue("hasLightmap", false);
            glDrawArrays(GL_LINES, 0, gridVertexCount);
            gridVbo.release();
        }

        if (iffBuffersDirty) {
            setupBuffers();
            iffBuffersDirty = false;
        }

        vbo.bind();
        program.enableAttributeArray(0);
        program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 8 * sizeof(float));
        program.enableAttributeArray(1);
        program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 2, 8 * sizeof(float));
        program.enableAttributeArray(2);
        program.setAttributeBuffer(2, GL_FLOAT, 5 * sizeof(float), 3, 8 * sizeof(float));

        bool lightmapAttrBound = false;
        if (hasLightmapUvs && lightmapUvVbo.isCreated()) {
            lightmapUvVbo.bind();
            program.enableAttributeArray(3);
            program.setAttributeBuffer(3, GL_FLOAT, 0, 2, 2 * sizeof(float));
            lightmapAttrBound = true;
        }

        model.rotate(rotX, 1.0f, 0.0f, 0.0f);
        model.rotate(rotY, 0.0f, 1.0f, 0.0f);
        model.rotate(rotZ, 0.0f, 0.0f, 1.0f);

        program.setUniformValue("projection", projection);
        program.setUniformValue("view", view);
        program.setUniformValue("model", model);

        if (submeshes.empty()) {
            program.setUniformValue("hasTexture", false);
            program.setUniformValue("hasLightmap", false);
            glDrawArrays(GL_TRIANGLES, 0, vertices.size() / 3);
        } else {
            for (const auto& sm : submeshes) {
                if (sm.count == 0) continue;
                if (sm.disableDepthTest)
                    glDisable(GL_DEPTH_TEST);
                if (sm.texture && sm.texture->isCreated()) {
                    sm.texture->bind();
                    program.setUniformValue("hasTexture", true);
                    program.setUniformValue("texture1", 0);
                } else {
                    program.setUniformValue("hasTexture", false);
                }

                if (lightmapAttrBound && sm.lightmapTexture && sm.lightmapTexture->isCreated()) {
                    glActiveTexture(GL_TEXTURE1);
                    sm.lightmapTexture->bind();
                    program.setUniformValue("hasLightmap", true);
                    program.setUniformValue("lightmapTex", 1);
                    glActiveTexture(GL_TEXTURE0);
                } else {
                    program.setUniformValue("hasLightmap", false);
                }

                if (sm.useOverrideColor) {
                    program.setUniformValue("useOverrideColor", true);
                    program.setUniformValue("overrideColor", sm.overrideColor);
                } else {
                    program.setUniformValue("useOverrideColor", false);
                }
                glDrawArrays(sm.drawMode, sm.startIndex, sm.count);
                if (lightmapAttrBound && sm.lightmapTexture && sm.lightmapTexture->isCreated()) {
                    glActiveTexture(GL_TEXTURE1);
                    sm.lightmapTexture->release();
                    glActiveTexture(GL_TEXTURE0);
                }
                if (sm.disableDepthTest)
                    glEnable(GL_DEPTH_TEST);
            }
        }

        if (lightmapAttrBound) {
            program.disableAttributeArray(3);
            lightmapUvVbo.release();
        }
        program.disableAttributeArray(0);
        program.disableAttributeArray(1);
        program.disableAttributeArray(2);
        vbo.release();
        program.release();
        
        if (gl && showWireframe) {
            gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QOpenGLWidget::resizeEvent(event);
        infoOverlay->setGeometry(10, 10, 300, height() - 90);
        coordsOverlay->setGeometry(0, height() - 72, width(), 72);
        statsOverlay->setGeometry(width() - 250, 10, 240, 60);
    }

    float wrapAngle(float angle) {
        angle = fmod(angle, 360.0f);
        if (angle < 0) angle += 360.0f;
        return angle;
    }

    void mousePressEvent(QMouseEvent *event) override { 
        lastPos = event->pos(); 
        if (currentGraph.valid && event->button() == Qt::LeftButton) {
            QMatrix4x4 projection, view, model;
            projection.perspective(45.0f, float(width()) / float(height() ? height() : 1), 0.1f, 100.0f);
            view.translate(transX, transY, -zoom);
            model.rotate(rotX, 1.0f, 0.0f, 0.0f);
            model.rotate(rotY, 0.0f, 1.0f, 0.0f);
            model.rotate(rotZ, 0.0f, 0.0f, 1.0f);
            
            QMatrix4x4 mvp = projection * view * model;
            bool invertible;
            QMatrix4x4 invMvp = mvp.inverted(&invertible);
            if (invertible) {
                float x = (2.0f * event->pos().x()) / width() - 1.0f;
                float y = 1.0f - (2.0f * event->pos().y()) / height();
                QVector4D rayStart(x, y, -1.0f, 1.0f);
                QVector4D rayEnd(x, y, 1.0f, 1.0f);
                rayStart = invMvp * rayStart; rayStart /= rayStart.w();
                rayEnd = invMvp * rayEnd; rayEnd /= rayEnd.w();
                QVector3D rayOrigin = rayStart.toVector3D();
                QVector3D rayDir = (rayEnd.toVector3D() - rayOrigin).normalized();
                
                float minT = std::numeric_limits<float>::max();
                int closestId = -1;
                for (const auto& node : currentGraph.nodes) {
                    float baseH = std::max(50.0f, graphMaxDim * 0.015f) * graphNodeScale;
                    float H = baseH * std::max(1.0f, (float)node.radius); 
                    QVector3D center = worldToNormalized(node.x, node.y, node.z + H);
                    QVector3D oc = rayOrigin - center;
                    float b = QVector3D::dotProduct(oc, rayDir);
                    float r = (H * 1.5f) / modelScale;
                    float c = QVector3D::dotProduct(oc, oc) - r * r;
                    float h = b * b - c;
                    if (h > 0.0f) {
                        float t = -b - sqrt(h);
                        if (t > 0.0f && t < minT) {
                            minT = t;
                            closestId = node.id;
                        }
                    }
                }
                
                if (closestId != -1) {
                    selectedGraphNodeId = closestId;
                    const GraphNode* n = GRAPH_FindNode(currentGraph, closestId);
                    if (n) {
                        int numLinks = 0;
                        for (auto& e : currentGraph.edges) {
                            if (e.node1 == n->id || e.node2 == n->id) numLinks++;
                        }
                        currentJsonInfo = QString("Node ID: %1\nPos: %2, %3, %4\nRadius: %5\nGamma: %6\nCriteria: %7\nConnected Links: %8")
                                       .arg(n->id).arg(n->x).arg(n->y).arg(n->z).arg(n->radius).arg(n->gamma)
                                       .arg(QString::fromStdString(n->criteria)).arg(numLinks);
                        infoOverlay->setPlainText(QString("=== Node Info ===\n%1").arg(currentJsonInfo));
                        if (!infoOverlay->isVisible()) infoOverlay->show();
                    }
                    generateGraphGeometry();
                    centerModel();
                    setupBuffers();
                    update();
                }
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        int dx = event->pos().x() - lastPos.x();
        int dy = event->pos().y() - lastPos.y();
        if (event->buttons() & Qt::LeftButton) {
            rotY = wrapAngle(rotY + dx);
            rotX = wrapAngle(rotX + dy);
        } else if (event->buttons() & Qt::RightButton) {
            transX += dx * 0.01f;
            transY -= dy * 0.01f;
        } else if (event->buttons() & Qt::MiddleButton) {
            rotZ = wrapAngle(rotZ + dx);
        }
        update();
        lastPos = event->pos();
        updateCoordsOverlay();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (currentGraph.valid) {
            if (event->key() == Qt::Key_N) {
                showGraphNodes = !showGraphNodes;
                generateGraphGeometry();
                centerModel();
                setupBuffers();
                update();
                return;
            }
            if (event->key() == Qt::Key_L) {
                showGraphLinks = !showGraphLinks;
                generateGraphGeometry();
                centerModel();
                setupBuffers();
                update();
                return;
            }
            if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
                if (event->modifiers() & Qt::ControlModifier) {
                    graphNodeScale *= 1.2f;
                    generateGraphGeometry(); centerModel(); setupBuffers(); update();
                    return;
                }
            }
            if (event->key() == Qt::Key_Minus) {
                if (event->modifiers() & Qt::ControlModifier) {
                    graphNodeScale /= 1.2f;
                    generateGraphGeometry(); centerModel(); setupBuffers(); update();
                    return;
                }
            }
        }
        QOpenGLWidget::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        if (currentGraph.valid && ((event->buttons() & Qt::MiddleButton) || (event->modifiers() & Qt::ControlModifier))) {
            if (event->angleDelta().y() > 0) {
                graphNodeScale *= 1.1f;
            } else if (event->angleDelta().y() < 0) {
                graphNodeScale /= 1.1f;
            }
            generateGraphGeometry();
            centerModel();
            setupBuffers();
            update();
            return;
        }

        zoom -= event->angleDelta().y() / 120.0f * 0.2f;
        if (zoom < 0.1f) zoom = 0.1f;
        update();
        updateCoordsOverlay();
    }

private:
    QVector<float> vertices;
    QVector<float> uvs;
    QVector<float> normals;
    QOpenGLShaderProgram program;
    QOpenGLBuffer vbo;
    QOpenGLBuffer gridVbo;
    int gridVertexCount = 0;
    std::unique_ptr<QOpenGLTexture> texture;

    // Lightmap UV2 channel - kept in a SEPARATE vbo from the main
    // pos/uv/normal interleaved buffer so "Apply Lightmap" never touches
    // the shared vertex layout used by every other view mode (graphs,
    // IFF animation, etc).  Populated by applyLightmapTextures(); only
    // covers the root mesh's vertices (attachments get zero UV2 - they
    // are separate child meshes, out of scope for this pass).
    QOpenGLBuffer lightmapUvVbo;
    bool hasLightmapUvs = false;

    QPoint lastPos;
    float rotX = 0, rotY = 0, rotZ = 0;
    float transX = 0, transY = 0;
    float zoom = 3.0f;
};

class ImageEditor : public QWidget {
public:
    ImageEditor(QWidget* parent = nullptr) : QWidget(parent) {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // ── Toolbar row (embedded in ImageEditor; MainWindow mirrors
        // these buttons into the unified top-of-screen viewer toolbar
        // so the user always sees them at the same position as the
        // 3D graph buttons).  The internal copy stays here so the
        // image-editor canvas still has its controls even when the
        // viewer is shown without the unified toolbar (e.g. the
        // embedded "Convert to TEX" / "Save As" action in the
        // context menu). ───────────────────────────────
        toolsWidget = new QWidget();
        toolsWidget->setStyleSheet("background:#2a2a2a; border-bottom:1px solid #444;");
        QHBoxLayout* tools = new QHBoxLayout(toolsWidget);
        tools->setContentsMargins(6, 4, 6, 4);
        tools->setSpacing(4);

        auto mkBtn = [](const QString& text, const QString& tip = "") {
            QPushButton* b = new QPushButton(text);
            b->setToolTip(tip);
            b->setFixedHeight(26);
            b->setStyleSheet("QPushButton{background:#3a3a3a;color:#ddd;border:1px solid #555;border-radius:3px;padding:2px 8px;}"
                             "QPushButton:hover{background:#4a4a4a;}"
                             "QPushButton:checked{background:#0066aa;border-color:#0088cc;}");
            b->setCheckable(true);
            return b;
        };
        auto mkBtnNorm = [](const QString& text, const QString& tip = "") {
            QPushButton* b = new QPushButton(text);
            b->setToolTip(tip);
            b->setFixedHeight(26);
            b->setStyleSheet("QPushButton{background:#3a3a3a;color:#ddd;border:1px solid #555;border-radius:3px;padding:2px 8px;}"
                             "QPushButton:hover{background:#4a4a4a;}");
            return b;
        };

        btnDraw    = mkBtn("✏ Draw",   "Toggle pencil tool [D]");
        btnEraser  = mkBtn("⌫ Erase",  "Toggle eraser tool [E]");
        QPushButton* btnColor  = mkBtnNorm("🎨 Color",  "Pick pen color");
        QPushButton* btnFlipH  = mkBtnNorm("↔ Flip H",  "Flip horizontally (and save with the flip)");
        QPushButton* btnFlipV  = mkBtnNorm("↕ Flip V",  "Flip vertically (and save with the flip)");
        QPushButton* btnFit    = mkBtnNorm("⊡ Fit",     "Fit to window");
        QPushButton* btnZoomIn = mkBtnNorm("+ Zoom",    "Zoom in");
        QPushButton* btnZoomOut= mkBtnNorm("- Zoom",    "Zoom out");
        QPushButton* btnClear  = mkBtnNorm("⟳ Reset",  "Reset to original");
        QPushButton* btnSave   = mkBtnNorm("💾 Save",  "Save (Ctrl+S)");

        // Pen size spinner
        QLabel* lblSize = new QLabel("Size:");
        lblSize->setStyleSheet("color:#aaa; font-size:11px;");
        QSpinBox* penSizeSpin = new QSpinBox();
        penSizeSpin->setRange(1, 40);
        penSizeSpin->setValue(3);
        penSizeSpin->setFixedWidth(50);
        penSizeSpin->setFixedHeight(24);
        penSizeSpin->setStyleSheet("background:#333;color:#ddd;border:1px solid #555;border-radius:2px;");

        // Info label
        infoLabel = new QLabel("No image");
        infoLabel->setStyleSheet("color:#888;font-size:10px;font-family:Consolas;");

        tools->addWidget(btnDraw);
        tools->addWidget(btnEraser);
        tools->addWidget(lblSize);
        tools->addWidget(penSizeSpin);
        tools->addWidget(btnColor);
        tools->addSpacing(10);
        tools->addWidget(btnFlipH);
        tools->addWidget(btnFlipV);
        tools->addSpacing(10);
        tools->addWidget(btnFit);
        tools->addWidget(btnZoomIn);
        tools->addWidget(btnZoomOut);
        tools->addSpacing(10);
        tools->addWidget(btnClear);
        tools->addWidget(btnSave);
        tools->addStretch();
        tools->addWidget(infoLabel);
        layout->addWidget(toolsWidget);

        // ── Scroll area for zoomable canvas ──────────────────
        scrollArea = new QScrollArea();
        scrollArea->setAlignment(Qt::AlignCenter);
        scrollArea->setStyleSheet("background:#1a1a1a; border:none;");
        imageLabel = new QLabel();
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        scrollArea->setWidget(imageLabel);
        scrollArea->setWidgetResizable(false);
        layout->addWidget(scrollArea, 1);
        
        // ── Connect buttons ───────────────────────────────────
        connect(btnDraw, &QPushButton::clicked, this, [this]() {
            if (btnDraw->isChecked()) { isDrawing = true; isErasing = false; btnEraser->setChecked(false); }
            else { isDrawing = false; }
        });
        connect(btnEraser, &QPushButton::clicked, this, [this]() {
            if (btnEraser->isChecked()) { isErasing = true; isDrawing = false; btnDraw->setChecked(false); }
            else { isErasing = false; }
        });
        connect(btnColor, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(penColor, this, "Pick Pen Color");
            if (c.isValid()) penColor = c;
        });
        // Flip H / Flip V bake the transform straight into currentImage
        // so the next Save writes the flipped pixels (no separate
        // "commit" step needed).  "Reset" still goes back to the
        // pristine file as loaded from disk, so the user can always
        // undo a flip.
        connect(btnFlipH, &QPushButton::clicked, this, [this]() {
            if (currentImage.isNull()) return;
            currentImage = currentImage.mirrored(true, false);
            updateDisplay();
        });
        connect(btnFlipV, &QPushButton::clicked, this, [this]() {
            if (currentImage.isNull()) return;
            currentImage = currentImage.mirrored(false, true);
            updateDisplay();
        });
        connect(btnFit, &QPushButton::clicked, this, [this]() {
            if (!currentImage.isNull()) {
                zoomFactor = std::min(
                    (double)scrollArea->width()  / currentImage.width(),
                    (double)scrollArea->height() / currentImage.height());
                updateDisplay();
            }
        });
        connect(btnZoomIn,  &QPushButton::clicked, this, [this]() { zoomFactor = qMin(zoomFactor * 1.25, 8.0); updateDisplay(); });
        connect(btnZoomOut, &QPushButton::clicked, this, [this]() { zoomFactor = qMax(zoomFactor / 1.25, 0.1); updateDisplay(); });
        connect(penSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { penSize = v; });
        connect(btnClear, &QPushButton::clicked, this, [this]() { currentImage = originalImage; updateDisplay(); });
        connect(btnSave, &QPushButton::clicked, this, [this]() {
            if (!currentPath.isEmpty() && !currentImage.isNull()) {
                bool isGameFormat = currentPath.endsWith(".tex", Qt::CaseInsensitive)
                                 || currentPath.endsWith(".spr", Qt::CaseInsensitive)
                                 || currentPath.endsWith(".pic", Qt::CaseInsensitive);
                if (isGameFormat) {
                    if (saveAsTex(currentImage, currentPath))
                        QMessageBox::information(this, "Saved", "Saved to game format:\n" + currentPath);
                    else
                        QMessageBox::critical(this, "Error", "Failed to save:\n" + currentPath);
                } else {
                    if (currentImage.save(currentPath))
                        QMessageBox::information(this, "Saved", "Saved:\n" + currentPath);
                    else
                        QMessageBox::critical(this, "Error", "Failed to save:\n" + currentPath);
                }
            }
        });
        
        imageLabel->installEventFilter(this);
        scrollArea->viewport()->installEventFilter(this);
    }

    
    // Write `img` to `outPath` as a LOOP v11 container.  When `mode` is
    // -1 (the default) the format is chosen from the filename: ".spr"
    // and ".pic" produce ARGB8888, anything else produces the legacy
    // 16-bit packed format.  Pass an explicit mode to override.
    bool saveAsTex(const QImage& img, const QString& outPath, int mode = -1) {
        if (img.isNull()) return false;
        if (img.width() > 65535 || img.height() > 65535) return false;

        // Make sure the destination directory exists.
        QFileInfo outInfo(outPath);
        QDir().mkpath(outInfo.absolutePath());

        if (mode < 0) {
            QString lower = outPath.toLower();
            mode = (lower.endsWith(".spr") || lower.endsWith(".pic") ||
                    lower.contains("argb8888")) ? 3 : 2;
        }

        // QImage::Format_ARGB32 is stored as 0xAARRGGBB in memory on
        // little-endian, i.e. bytes are B, G, R, A.  We re-pack into
        // a contiguous RGBA8888 buffer that the shared LOOP encoder
        // consumes directly.
        QImage texImg = img.convertToFormat(QImage::Format_ARGB32);
        std::vector<uint8_t> rgba(static_cast<size_t>(texImg.width()) * texImg.height() * 4);
        for (int y = 0; y < texImg.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(texImg.constScanLine(y));
            for (int x = 0; x < texImg.width(); ++x) {
                size_t o = (static_cast<size_t>(y) * texImg.width() + x) * 4;
                rgba[o + 0] = static_cast<uint8_t>(qRed(row[x]));
                rgba[o + 1] = static_cast<uint8_t>(qGreen(row[x]));
                rgba[o + 2] = static_cast<uint8_t>(qBlue(row[x]));
                rgba[o + 3] = static_cast<uint8_t>(qAlpha(row[x]));
            }
        }
        return TEX_WriteLOOP(outPath.toStdString(),
                             rgba.data(), texImg.width(), texImg.height(),
                             mode, 11);
    }
    
    void loadImage(const QString& path, const QImage& img) {
        currentPath = path;
        originalImage = img;
        currentImage = img;
        // Default to the same fit-to-window view as the Fit toolbar button.
        if (!img.isNull() && scrollArea) {
            double fx = static_cast<double>(scrollArea->width())  / img.width();
            double fy = static_cast<double>(scrollArea->height()) / img.height();
            zoomFactor = qMin(fx, fy);
        } else {
            zoomFactor = 1.0;
        }
        updateDisplay();
        if (infoLabel) {
            infoLabel->setText(QString("%1x%2 | %3")
                .arg(img.width()).arg(img.height())
                .arg(QFileInfo(path).fileName()));
        }
    }
    
    void clear() {
        imageLabel->clear();
        currentPath.clear();
        currentImage = QImage();
        if (infoLabel) infoLabel->setText("No image");
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            QWheelEvent* we = static_cast<QWheelEvent*>(event);
            if (we->angleDelta().y() > 0) {
                zoomFactor = qMin(zoomFactor * 1.25, 8.0);
            } else {
                zoomFactor = qMax(zoomFactor / 1.25, 0.1);
            }
            updateDisplay();
            return true;
        }

        // Drawing on the image label
        if (obj == imageLabel && (isDrawing || isErasing) && !currentImage.isNull()) {
            if (event->type() == QEvent::MouseButtonPress) {
                QMouseEvent* me = static_cast<QMouseEvent*>(event);
                pushUndo();
                lastPoint = getMappedPoint(me->pos());
                drawingNow = true;
                return true;
            } else if (event->type() == QEvent::MouseMove) {
                QMouseEvent* me = static_cast<QMouseEvent*>(event);
                if (drawingNow) {
                    QPoint pt = getMappedPoint(me->pos());
                    QPainter painter(&currentImage);
                    painter.setRenderHint(QPainter::Antialiasing);
                    if (isErasing) {
                        // Restore original pixels along the erased line
                        int r = penSize;
                        for (int dy = -r; dy <= r; dy++) {
                            for (int dx = -r; dx <= r; dx++) {
                                if (dx*dx + dy*dy <= r*r) {
                                    // Sample along the line from lastPoint to pt
                                    int steps = std::max(std::abs(pt.x()-lastPoint.x()), std::abs(pt.y()-lastPoint.y())) + 1;
                                    for (int s = 0; s <= steps; s++) {
                                        int cx = lastPoint.x() + (pt.x()-lastPoint.x())*s/steps + dx;
                                        int cy = lastPoint.y() + (pt.y()-lastPoint.y())*s/steps + dy;
                                        if (cx >= 0 && cy >= 0 && cx < currentImage.width() && cy < currentImage.height())
                                            currentImage.setPixel(cx, cy, originalImage.pixel(cx, cy));
                                    }
                                }
                            }
                        }
                        lastPoint = pt;
                        updateDisplay();
                        return true;
                    } else {
                        painter.setPen(QPen(penColor, penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    }
                    painter.drawLine(lastPoint, pt);
                    lastPoint = pt;
                    updateDisplay();
                }
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease) {
                drawingNow = false;
                return true;
            }
        }
        // Zoom with Ctrl+Wheel on scroll viewport
        if (obj == scrollArea->viewport() && event->type() == QEvent::Wheel) {
            QWheelEvent* we = static_cast<QWheelEvent*>(event);
            if (we->modifiers() & Qt::ControlModifier) {
                double factor = (we->angleDelta().y() > 0) ? 1.15 : (1.0/1.15);
                zoomFactor = qBound(0.1, zoomFactor * factor, 8.0);
                updateDisplay();
                return true;
            }
        }
        return QWidget::eventFilter(obj, event);
    }

    // Exposed so MainWindow can reparent the image toolbar into the
    // unified viewer toolbar row (Row 2, below "Mode: ...") so the
    // image buttons sit at the same screen position as the 3D graph
    // buttons and the IFF media bar.
    QWidget* toolsWidget = nullptr;

protected:
    void resizeEvent(QResizeEvent* event) override {
        updateDisplay();
        QWidget::resizeEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Undo)) {
            imgUndo();
        } else if (event->matches(QKeySequence::Redo)) {
            imgRedo();
        } else if (event->key() == Qt::Key_Delete) {
            pushUndo();
            currentImage.fill(Qt::transparent);
            updateDisplay();
        } else if (event->matches(QKeySequence::Copy)) {
            if (!currentImage.isNull())
                QApplication::clipboard()->setImage(currentImage);
        } else if (event->matches(QKeySequence::Paste)) {
            QImage img = QApplication::clipboard()->image();
            if (!img.isNull()) {
                pushUndo();
                currentImage = img;
                updateDisplay();
            }
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    void updateDisplay() {
        if (currentImage.isNull()) return;
        int dw = (int)(currentImage.width()  * zoomFactor);
        int dh = (int)(currentImage.height() * zoomFactor);
        imageLabel->setFixedSize(dw, dh);
        imageLabel->setPixmap(QPixmap::fromImage(currentImage).scaled(dw, dh, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QPoint getMappedPoint(const QPoint& p) {
        if (currentImage.isNull()) return p;
        int dw = (int)(currentImage.width()  * zoomFactor);
        int dh = (int)(currentImage.height() * zoomFactor);
        int x = (int)((double)p.x() / dw * currentImage.width());
        int y = (int)((double)p.y() / dh * currentImage.height());
        return QPoint(qBound(0,x,currentImage.width()-1), qBound(0,y,currentImage.height()-1));
    }

    void pushUndo() {
        if (!currentImage.isNull()) {
            undoStack.append(currentImage);
            if (undoStack.size() > 50) undoStack.removeFirst();
            redoStack.clear();
        }
    }

    void imgUndo() {
        if (!undoStack.isEmpty()) {
            redoStack.append(currentImage);
            currentImage = undoStack.takeLast();
            updateDisplay();
        }
    }

    void imgRedo() {
        if (!redoStack.isEmpty()) {
            undoStack.append(currentImage);
            currentImage = redoStack.takeLast();
            updateDisplay();
        }
    }

    QLabel*      imageLabel  = nullptr;
    QLabel*      infoLabel   = nullptr;
    QScrollArea* scrollArea  = nullptr;
    QPushButton* btnDraw     = nullptr;
    QPushButton* btnEraser   = nullptr;
    QImage originalImage;
    QImage currentImage;
    QString currentPath;
    bool isDrawing  = false;
    bool isErasing  = false;
    bool drawingNow = false;
    QPoint lastPoint;
    QColor penColor = Qt::red;
    int    penSize  = 3;
    double zoomFactor = 1.0;
    QList<QImage> undoStack;
    QList<QImage> redoStack;
};


class QscSyntaxHighlighter : public QSyntaxHighlighter {
public:
    QscSyntaxHighlighter(QTextDocument *parent = nullptr) : QSyntaxHighlighter(parent) {
        HighlightingRule rule;
        keywordFormat.setForeground(QColor("#569CD6"));
        keywordFormat.setFontWeight(QFont::Bold);
        QStringList keywordPatterns = {
            "\\bint\\b", "\\bfloat\\b", "\\bvoid\\b", "\\bif\\b", "\\belse\\b",
            "\\bwhile\\b", "\\bfor\\b", "\\breturn\\b", "\\bTRUE\\b", "\\bFALSE\\b",
            "\\bstring\\b"
        };
        for (const QString &pattern : keywordPatterns) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = keywordFormat;
            highlightingRules.append(rule);
        }

        classFormat.setForeground(QColor("#4EC9B0"));
        classFormat.setFontWeight(QFont::Bold);
        rule.pattern = QRegularExpression("\\bQ[A-Za-z]+\\b");
        rule.format = classFormat;
        highlightingRules.append(rule);

        singleLineCommentFormat.setForeground(QColor("#6A9955"));
        rule.pattern = QRegularExpression("//[^\n]*");
        rule.format = singleLineCommentFormat;
        highlightingRules.append(rule);

        multiLineCommentFormat.setForeground(QColor("#6A9955"));
        commentStartExpression = QRegularExpression("/\\*");
        commentEndExpression = QRegularExpression("\\*/");

        quotationFormat.setForeground(QColor("#CE9178"));
        rule.pattern = QRegularExpression("\".*\"");
        rule.format = quotationFormat;
        highlightingRules.append(rule);

        functionFormat.setFontItalic(true);
        functionFormat.setForeground(QColor("#DCDCAA"));
        rule.pattern = QRegularExpression("\\b[A-Za-z0-9_]+(?=\\()");
        rule.format = functionFormat;
        highlightingRules.append(rule);
    }

protected:
    void highlightBlock(const QString &text) override {
        for (const HighlightingRule &rule : highlightingRules) {
            QRegularExpressionMatchIterator matchIterator = rule.pattern.globalMatch(text);
            while (matchIterator.hasNext()) {
                QRegularExpressionMatch match = matchIterator.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }
        setCurrentBlockState(0);
        int startIndex = 0;
        if (previousBlockState() != 1) startIndex = text.indexOf(commentStartExpression);
        while (startIndex >= 0) {
            QRegularExpressionMatch match = commentEndExpression.match(text, startIndex);
            int endIndex = match.capturedStart();
            int commentLength = 0;
            if (endIndex == -1) {
                setCurrentBlockState(1);
                commentLength = text.length() - startIndex;
            } else {
                commentLength = endIndex - startIndex + match.capturedLength();
            }
            setFormat(startIndex, commentLength, multiLineCommentFormat);
            startIndex = text.indexOf(commentStartExpression, startIndex + commentLength);
        }
    }

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<HighlightingRule> highlightingRules;
    QRegularExpression commentStartExpression;
    QRegularExpression commentEndExpression;
    QTextCharFormat keywordFormat;
    QTextCharFormat classFormat;
    QTextCharFormat singleLineCommentFormat;
    QTextCharFormat multiLineCommentFormat;
    QTextCharFormat quotationFormat;
    QTextCharFormat functionFormat;
};

class CodeEditor : public QTextEdit {
public:
    CodeEditor(QWidget *parent = nullptr) : QTextEdit(parent) {
        highlighter = new QscSyntaxHighlighter(this->document());
        QFile file(QCoreApplication::applicationDirPath() + "/IGIAutoComplete.txt");
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (!line.isEmpty()) autocompleteList.append(line);
            }
        }
        QFile modelFile(QCoreApplication::applicationDirPath() + "/IGIModels.json");
        if (modelFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonDocument doc = QJsonDocument::fromJson(modelFile.readAll());
            modelInfoObj = doc.object();
        }
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this, &CodeEditor::showCustomContextMenu);
    }

    bool isHexMode = false;
    
protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (isHexMode) {
            if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right || e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
                QTextEdit::keyPressEvent(e);
                return;
            }
            if (e->text().isEmpty()) {
                QTextEdit::keyPressEvent(e);
                return;
            }
            if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
                return;
            }
            
            QTextCursor cursor = textCursor();
            int col = cursor.positionInBlock();
            QString text = e->text();
            QChar inputChar = text[0];
            
            if (col >= 60 && col < 76) {
                // Editing ASCII
                char c = inputChar.toLatin1();
                QString hexStr = QString("%1").arg((quint8)c, 2, 16, QChar('0')).toUpper();
                
                int hexPos = 10 + (col - 60) * 3;
                cursor.setPosition(cursor.block().position() + hexPos);
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
                cursor.insertText(hexStr);
                
                cursor.setPosition(cursor.block().position() + col);
                cursor.deleteChar();
                cursor.insertText(QString((c >= 32 && c < 127) ? c : '.'));
                
                cursor.setPosition(cursor.block().position() + col + 1);
                setTextCursor(cursor);
            } else if (col >= 10 && col < 58) {
                // Editing HEX
                if (!inputChar.isLetterOrNumber() || !QString("0123456789ABCDEFabcdef").contains(inputChar)) return;
                
                inputChar = inputChar.toUpper();
                cursor.deleteChar();
                cursor.insertText(inputChar);
                
                int byteStartCol = 10 + ((col - 10) / 3) * 3;
                cursor.setPosition(cursor.block().position() + byteStartCol);
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
                QString byteHex = cursor.selectedText();
                
                char c = (char)byteHex.toInt(nullptr, 16);
                
                int asciiPos = 60 + (byteStartCol - 10) / 3;
                cursor.setPosition(cursor.block().position() + asciiPos);
                cursor.deleteChar();
                cursor.insertText(QString((c >= 32 && c < 127) ? c : '.'));
                
                cursor.setPosition(cursor.block().position() + col + 1);
                if ((col - 10) % 3 == 1) cursor.movePosition(QTextCursor::Right);
                setTextCursor(cursor);
            }
            return;
        }

        if (e->key() == Qt::Key_Tab) {
            QTextCursor cursor = textCursor();
            cursor.select(QTextCursor::WordUnderCursor);
            QString prefix = cursor.selectedText();
            if (!prefix.isEmpty()) {
                for (const QString &word : autocompleteList) {
                    if (word.startsWith(prefix, Qt::CaseInsensitive) && word != prefix) {
                        cursor.insertText(word);
                        return;
                    }
                }
            }
        }
        QTextEdit::keyPressEvent(e);
    }
    
private:
    void showCustomContextMenu(const QPoint &pt) {
        QMenu *menu = createStandardContextMenu();
        QTextCursor cursor = cursorForPosition(pt);
        cursor.select(QTextCursor::WordUnderCursor);
        QString word = cursor.selectedText();
        
        QRegularExpression re("\\d{3}_\\d{2}_\\d");
        if (re.match(word).hasMatch()) {
            menu->addSeparator();
            QAction *infoAction = menu->addAction("Model Information: " + word);
            connect(infoAction, &QAction::triggered, [this, word]() {
                QString info = "No information found for this model in IGIModels.json.";
                
                // Parse modelInfoObj which might be an array or object
                if (modelInfoObj.contains(word)) {
                    QJsonDocument doc(modelInfoObj[word].toObject());
                    info = doc.toJson(QJsonDocument::Indented);
                } else {
                    // It was an array inside a JSON file but loaded as an array? Wait, I didn't load it as an array!
                    // I will check the JSON directly.
                    QFile modelFile(QCoreApplication::applicationDirPath() + "/IGIModels.json");
                    if (modelFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        QJsonDocument fullDoc = QJsonDocument::fromJson(modelFile.readAll());
                        if (fullDoc.isArray()) {
                            QJsonArray arr = fullDoc.array();
                            for (int i = 0; i < arr.size(); ++i) {
                                QJsonObject obj = arr[i].toObject();
                                if (obj["ModelId"].toString() == word) {
                                    info = "Model Name: " + obj["ModelName"].toString();
                                    break;
                                }
                            }
                        } else if (fullDoc.isObject() && fullDoc.object().contains(word)) {
                            info = "Model Name: " + fullDoc.object()[word].toObject()["ModelName"].toString();
                        }
                    }
                }
                QMessageBox::information(this, "Model Info", info);
            });
        }
        menu->exec(mapToGlobal(pt));
        delete menu;
    }

    QscSyntaxHighlighter* highlighter;
    QStringList autocompleteList;
    QJsonObject modelInfoObj;
};

// ────────────────────────────────────────────────────────────────────────────
// GraphHexEditor - read-only hex viewer tailored for IGI navigation
// graph .dat files.  Mirrors the layout of the legacy IGI Graph
// Editor: 16-byte-wide hex view on the left, info panel on the
// right.  Only instantiated for graph*.dat files (the loadFile()
// flow routes graph.dat here when the user selects Hex mode).
// ────────────────────────────────────────────────────────────────────────────
class GraphHexEditor : public QWidget {
public:
    explicit GraphHexEditor(QWidget* parent = nullptr) : QWidget(parent) {
        QHBoxLayout* root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        // ── Left: hex view ──
        QWidget* leftPanel = new QWidget();
        leftPanel->setStyleSheet("background:#1a1a1a; color:#9cdcfe; font-family:Consolas;");
        QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->setSpacing(0);

        // Header: address offsets 0..F
        QLabel* hdr = new QLabel("    00 01 02 03 04 05 06 07   08 09 0A 0B 0C 0D 0E 0F  | Text");
        hdr->setStyleSheet("color:#6a9955; background:#0a0a0a; padding:2px 4px; font-family:Consolas;");
        leftLayout->addWidget(hdr);

        // Hex rows: one QLabel per row keeps this simple and read-only.
        m_hexView = new QTextEdit();
        m_hexView->setReadOnly(true);
        m_hexView->setStyleSheet(
            "QTextEdit { background:#1a1a1a; color:#9cdcfe; border:none;"
            "  font-family:Consolas; font-size:11px; }");
        leftLayout->addWidget(m_hexView, 1);

        root->addWidget(leftPanel, 3);

        // ── Right: info panel ──
        QWidget* rightPanel = new QWidget();
        rightPanel->setFixedWidth(260);
        rightPanel->setStyleSheet("background:#252526; color:#cfd; font-family:Consolas;");
        QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
        rightLayout->setContentsMargins(8, 8, 8, 8);
        rightLayout->setSpacing(6);

        auto addField = [&](const QString& label, QLabel** out) {
            QLabel* l = new QLabel(label);
            l->setStyleSheet("color:#9cdcfe; font-size:11px;");
            rightLayout->addWidget(l);
            QLabel* v = new QLabel("-");
            v->setStyleSheet("color:#fff; font-size:12px; font-weight:bold; padding:2px 4px;"
                              " background:#333; border:1px solid #555; border-radius:3px;");
            v->setWordWrap(true);
            rightLayout->addWidget(v);
            *out = v;
        };
        addField("Max Nodes",    &m_maxNodes);
        addField("Signature",    &m_signature);
        addField("Data-Type",    &m_dataType);
        addField("Node #",       &m_nodeId);
        addField("Graph Data",   &m_graphData);

        // Dropdown for graph items (Item, Signature, Datatype, Datasize)
        // mirrors the legacy IGI Graph Editor's `graphHexItemsDD`
        // ComboBox.
        QLabel* ddLabel = new QLabel("Item:");
        ddLabel->setStyleSheet("color:#9cdcfe; font-size:11px;");
        rightLayout->addWidget(ddLabel);
        m_itemCombo = new QComboBox();
        m_itemCombo->addItems({"Item", "Signature", "Datatype", "Datasize", "Node data", "Edge data"});
        rightLayout->addWidget(m_itemCombo);

        m_itemLabel = new QLabel("-");
        m_itemLabel->setStyleSheet("color:#fff; font-size:11px; padding:4px;"
                                    " background:#333; border:1px solid #555; border-radius:3px;");
        m_itemLabel->setWordWrap(true);
        rightLayout->addWidget(m_itemLabel);

        // Checkboxes (Reset Data, Auto Format, Edit Mode) for parity
        // with the legacy editor.
        m_resetData = new QCheckBox("Reset Data");
        m_autoFormat = new QCheckBox("Auto Format");
        m_autoFormat->setChecked(true);
        m_editMode = new QCheckBox("Edit Mode");
        rightLayout->addWidget(m_resetData);
        rightLayout->addWidget(m_autoFormat);
        rightLayout->addWidget(m_editMode);
        rightLayout->addStretch();

        root->addWidget(rightPanel, 1);
    }

    // Load a graph .dat file and rebuild the hex view + info panel.
    void loadFile(const QString& path) {
        m_path = path;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            m_hexView->setPlainText("(could not open " + path + ")");
            return;
        }
        QByteArray data = f.readAll();
        m_data = data;

        // Build a 16-byte-wide hex view similar to the legacy IGI
        // Graph Editor's `UpdateHexInfoPanel` / `HexByteClick` flow.
        QString out;
        int rows = (data.size() + 15) / 16;
        for (int i = 0; i < rows; ++i) {
            int off = i * 16;
            out += QString("%1  ").arg(off, 8, 16, QChar('0')).toUpper();
            // First 8 bytes
            for (int n = 0; n < 8; ++n) {
                if (off + n < data.size())
                    out += QString("%1 ").arg((quint8)data[off + n], 2, 16, QChar('0')).toUpper();
                else
                    out += "   ";
            }
            out += " ";
            // Last 8 bytes
            for (int n = 8; n < 16; ++n) {
                if (off + n < data.size())
                    out += QString("%1 ").arg((quint8)data[off + n], 2, 16, QChar('0')).toUpper();
                else
                    out += "   ";
            }
            out += " | ";
            // ASCII text
            for (int n = 0; n < 16; ++n) {
                if (off + n < data.size()) {
                    char c = data[off + n];
                    out += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
                } else {
                    out += " ";
                }
            }
            out += "\n";
        }
        m_hexView->setPlainText(out);

        // ── Info panel ──
        // Signature: first 4 bytes of the file
        QString sig = "";
        for (int i = 0; i < 4 && i < data.size(); ++i) {
            sig += QString("%1 ").arg((quint8)data[i], 2, 16, QChar('0')).toUpper();
        }
        m_signature->setText(sig.trimmed());

        // Max Nodes at offset 4-7 (uint32 LE)
        int maxNodes = 0;
        if (data.size() >= 8) {
            maxNodes = (quint8)data[4] | ((quint8)data[5] << 8)
                     | ((quint8)data[6] << 16) | ((quint8)data[7] << 24);
        }
        m_maxNodes->setText(QString::number(maxNodes));

        // Graph Data: total file size in bytes (Datasize)
        m_graphData->setText(QString::number(data.size()) + " bytes");

        // Data-Type: "Integer" (since the node IDs and table are
        // little-endian uint32, the dominant data type is "Integer").
        m_dataType->setText("Integer");

        // Node # at the currently selected byte (start with 0)
        m_nodeId->setText("0");
        m_lastClickedByte = 0;

        // Wire the hex view so a click selects a byte and the info
        // panel updates to show what type of record lives at that
        // offset (mirrors `IGIGraphEditorUI.HexByteClick`).
        connect(m_hexView, &QTextEdit::cursorPositionChanged, this, [this, data]() {
            QTextCursor cur = m_hexView->textCursor();
            int block = cur.blockNumber();
            int col   = cur.columnNumber();
            int byteOff = block * 16;
            if (col >= 0 && col < 3) {
                // First hex column group: 0..7
                int idx = (col - 0) / 3;
                if (idx >= 0 && idx < 8) byteOff += idx;
            } else if (col >= 19 && col < 22) {
                // Second hex column group: 8..F  (after the gap)
                int idx = (col - 19) / 3;
                if (idx >= 0 && idx < 8) byteOff += 8 + idx;
            }
            if (byteOff >= data.size()) return;
            m_lastClickedByte = byteOff;
            updateInfoForByte(data, byteOff);
        });

        // Wire the item dropdown so the user can switch between
        // Item / Signature / Datatype / Datasize / Node data / Edge
        // data and see the corresponding value for the byte at the
        // current cursor position (mirrors
        // `graphHexItemsDD_SelectedIndexChanged`).
        connect(m_itemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, data](int) { updateItemPanel(data, m_lastClickedByte); });
        connect(m_resetData,  &QCheckBox::toggled, this, [this](bool on) {
            if (on) { m_editMode->setChecked(false); m_autoFormat->setChecked(false); }
        });
        connect(m_editMode,  &QCheckBox::toggled, this, [this](bool on) {
            if (on) { m_resetData->setChecked(false); m_autoFormat->setChecked(true); }
        });

        updateInfoForByte(data, 0);
        updateItemPanel(data, 0);
    }

private:
    void updateInfoForByte(const QByteArray& data, int byteOff) {
        // Decode the uint32 little-endian value at this offset and
        // show it as a node # (if it looks like a valid index in the
        // range [0, maxNodes)) or as raw "Value: <hex>" otherwise.
        if (byteOff + 4 > data.size()) return;
        quint32 v = (quint8)data[byteOff]
                  | ((quint8)data[byteOff + 1] << 8)
                  | ((quint8)data[byteOff + 2] << 16)
                  | ((quint8)data[byteOff + 3] << 24);
        QString hex = QString::number(v, 16).toUpper();
        int maxNodes = m_maxNodes->text().toInt();
        if (maxNodes > 0 && v < (quint32)maxNodes) {
            m_nodeId->setText(QString::number(v));
        } else {
            m_nodeId->setText("0x" + hex + " (out of node range)");
        }
    }

    void updateItemPanel(const QByteArray& data, int byteOff) {
        // Mirrors `graphHexItemsDD_SelectedIndexChanged` in the
        // legacy IGI Graph Editor.
        int idx = m_itemCombo->currentIndex();
        if (byteOff + 4 > data.size()) {
            m_itemLabel->setText("(offset out of range)");
            return;
        }
        quint32 v = (quint8)data[byteOff]
                  | ((quint8)data[byteOff + 1] << 8)
                  | ((quint8)data[byteOff + 2] << 16)
                  | ((quint8)data[byteOff + 3] << 24);
        QString valHex = QString("0x%1").arg(v, 8, 16, QChar('0')).toUpper().right(8);
        QString out;
        switch (idx) {
        case 0: out = "Item: " + QString::number(v); break;
        case 1: out = "Signature: " + valHex; break;
        case 2: out = "Datatype: Integer (" + valHex + ")"; break;
        case 3: out = "Datasize: " + QString::number(v); break;
        case 4: out = "Node data @0x" + QString::number(byteOff, 16).toUpper().right(4)
                    + " = " + valHex; break;
        case 5: out = "Edge data @0x" + QString::number(byteOff, 16).toUpper().right(4)
                    + " = " + valHex; break;
        }
        m_itemLabel->setText(out);
    }

    QString     m_path;
    QByteArray  m_data;
    int         m_lastClickedByte = 0;
    QTextEdit*  m_hexView   = nullptr;
    QLabel*     m_maxNodes  = nullptr;
    QLabel*     m_signature = nullptr;
    QLabel*     m_dataType  = nullptr;
    QLabel*     m_nodeId    = nullptr;
    QLabel*     m_graphData = nullptr;
    QLabel*     m_itemLabel  = nullptr;
    QComboBox*  m_itemCombo = nullptr;
    QCheckBox*  m_resetData = nullptr;
    QCheckBox*  m_autoFormat = nullptr;
    QCheckBox*  m_editMode  = nullptr;
};

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        g_logger = [this](const QString& msg, LogLevel level) {
            this->logMessage(msg, level);
        };
        setWindowTitle("IGI Game Converter");
        QIcon appIcon(":/igi1conv.ico");
        setWindowIcon(appIcon);
        resize(1200, 800);
        treeView = nullptr;

        fileModel = new QFileSystemModel(this);
        fileModel->setReadOnly(false);
        QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
        // Fall back to the user's home directory; the previous
        // default was a machine-specific absolute path.
        QString defaultFolder = QDir::homePath();
        QString lastFolder = QSettings(iniPath, QSettings::IniFormat).value("LastFolder", defaultFolder).toString();
        if (!QDir(lastFolder).exists()) {
            lastFolder = defaultFolder;
        }
        fileModel->setRootPath(lastFolder);
        
        // Custom proxy model to handle type names and size formatting
        class MyProxyModel : public QSortFilterProxyModel {
        public:
            MyProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
            QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
                if (role == Qt::DisplayRole) {
                    int col = index.column();
                    QFileSystemModel* fsm = qobject_cast<QFileSystemModel*>(sourceModel());
                    if (fsm) {
                        QFileInfo info = fsm->fileInfo(mapToSource(index));
                        if (col == 1) { // Size
                            if (info.isFile()) {
                                qint64 size = info.size();
                                if (size < 1024) return QString::number(size) + " B";
                                if (size < 1024 * 1024) return QString::number(size / 1024.0, 'f', 1) + " KB";
                                return QString::number(size / (1024.0 * 1024.0), 'f', 1) + " MB";
                            }
                            return "";
                        } else if (col == 2) { // Type
                            if (info.isDir()) return "File Folder";
                            QString ext = info.suffix().toLower();
                            if (ext == "mef") return "MEF Binary Model";
                            if (ext == "mex") return "MEX Extended Model";
                            if (ext == "res") return "Resource Archive";
                            if (ext == "spr") return "Sprite Image";
                            if (ext == "fnt") return "Font File";
                            if (ext == "pic") return "Picture Image";
                            if (ext == "tex") return "Texture Image";
                            if (ext == "mtp") return "Material Properties";
                            if (ext == "txt") {
                                // Distinguish text MEF from generic text
                                if (info.fileName().contains(".mef.", Qt::CaseInsensitive))
                                    return "MEF Text Model";
                                return "Text File";
                            }
                            return ext.toUpper() + " File";
                        }
                    }
                }
                return QSortFilterProxyModel::data(index, role);
            }
        };

        proxyModel = new MyProxyModel(this);
        proxyModel->setSourceModel(fileModel);
        proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        proxyModel->setFilterKeyColumn(0); // Only filter by file name

        // ─── Model Search Toolbar ───────────────────────────────────────────
        QToolBar* searchToolBar = addToolBar("Model Search");
        searchToolBar->setMovable(false);
        searchToolBar->setVisible(false);
        searchToolBar->setStyleSheet("QToolBar{background:#252525;border-bottom:1px solid #444;spacing:4px;padding:2px 6px;}");
        
        QLabel* lblSearch = new QLabel("🔍 Model:");
        lblSearch->setStyleSheet("color:#ccc;font-size:11px;");
        searchToolBar->addWidget(lblSearch);
        
        QLineEdit* modelSearchBox = new QLineEdit();
        modelSearchBox->setPlaceholderText("Search by name or ID (e.g. 'Jones' or '000_01_1' or '435')...");
        modelSearchBox->setMinimumWidth(380);
        modelSearchBox->setMaximumWidth(500);
        modelSearchBox->setFixedHeight(24);
        modelSearchBox->setStyleSheet("background:#333;color:#eee;border:1px solid #555;border-radius:3px;padding:2px 6px;font-family:Consolas;font-size:11px;");
        searchToolBar->addWidget(modelSearchBox);
        
        QPushButton* btnRefresh = new QPushButton("Refresh");
        btnRefresh->setStyleSheet("background:#444;color:#eee;border:1px solid #555;border-radius:3px;padding:2px 8px;font-size:11px;");
        searchToolBar->addWidget(btnRefresh);
        connect(btnRefresh, &QPushButton::clicked, this, [this]() {
            if (this->fileModel) {
                QString currentPath = this->fileModel->rootPath();
                this->fileModel->setRootPath("");
                this->fileModel->setRootPath(currentPath);
            }
        });
        
        QLabel* modelSearchResult = new QLabel("  Type to search...");
        modelSearchResult->setStyleSheet("color:#888;font-size:11px;font-family:Consolas;min-width:300px;");
        searchToolBar->addWidget(modelSearchResult);
        
        // Load IGIModels.json for bidirectional search
        connect(modelSearchBox, &QLineEdit::textChanged, this, [this, modelSearchResult, iniPath](const QString& query) {
            if (query.trimmed().isEmpty()) {
                modelSearchResult->setText("  Type to search...");
                modelSearchResult->setStyleSheet("color:#888;font-size:11px;font-family:Consolas;min-width:300px;");
                proxyModel->setFilterWildcard("*");
                if (this->treeView) {
                    QString currentRoot = fileModel->rootPath();
                    if (currentRoot.isEmpty()) currentRoot = QDir::currentPath();
                    this->treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(currentRoot)));
                }
                return;
            }
            
            static QJsonArray s_modelsArr;
            static bool s_loaded = false;
            if (!s_loaded) {
                s_loaded = true;
                QFile f(QCoreApplication::applicationDirPath() + "/IGIModels.json");
                if (f.open(QIODevice::ReadOnly)) {
                    s_modelsArr = QJsonDocument::fromJson(f.readAll()).array();
                }
            }
            
            QString q = query.trimmed().toLower();
            QStringList results;
            QString exactMatchId;
            
            for (const QJsonValue& v : s_modelsArr) {
                QJsonObject obj = v.toObject();
                QString id   = obj["ModelId"].toString();
                QString name = obj["ModelName"].toString();
                
                bool exactMatch = (id.toLower() == q || name.toLower() == q);
                bool partialMatch = (id.toLower().contains(q) || name.toLower().contains(q));
                
                if (exactMatch) {
                    exactMatchId = id;
                    results.prepend(QString("%1 → %2").arg(id, name));
                } else if (partialMatch) {
                    if (exactMatchId.isEmpty()) exactMatchId = id; // use first match as fallback
                    results << QString("%1 → %2").arg(id, name);
                }
            }
            
            if (results.isEmpty()) {
                modelSearchResult->setText("  No results for: " + query);
                modelSearchResult->setStyleSheet("color:#ff6666;font-size:11px;font-family:Consolas;min-width:300px;");
                proxyModel->setFilterWildcard("*" + query + "*");
            } else {
                modelSearchResult->setText("  " + results.join("  |  "));
                modelSearchResult->setStyleSheet("color:#66FFAA;font-size:11px;font-family:Consolas;min-width:300px;");
                // Filter file tree to show all files (models + textures) belonging to the match
                QString basePrefix = exactMatchId.split('_').first();
                proxyModel->setFilterWildcard("*" + basePrefix + "*");
                if (this->treeView) {
                    QString currentRoot = fileModel->rootPath();
                    if (currentRoot.isEmpty()) currentRoot = QDir::currentPath();
                    this->treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(currentRoot)));
                }
            }
        });
        
        QShortcut *modelSearchToggle = new QShortcut(QKeySequence("Ctrl+Shift+M"), this);
        connect(modelSearchToggle, &QShortcut::activated, this, [this, searchToolBar, modelSearchBox]() {
            searchToolBar->setVisible(!searchToolBar->isVisible());
            if (searchToolBar->isVisible()) {
                modelSearchBox->setFocus();
                modelSearchBox->selectAll();
            }
        });

        QMenu* fileMenu = menuBar()->addMenu("&File");
        fileMenu->addAction(QIcon::fromTheme("document-open"), "&Open File...", this, [this]() {
            QString filePath = QFileDialog::getOpenFileName(this, "Select File to Open");
            if (!filePath.isEmpty()) {
                // Opening a file only loads it in the editor.  We deliberately
                // do NOT change the main workspace folder so the tree root
                // stays where the user set it.
                loadFile(filePath);
            }
        }, QKeySequence("Ctrl+O"));

        fileMenu->addAction(QIcon::fromTheme("folder-open"), "&Open Folder...", this, [this, iniPath]() {
            QString dir = QFileDialog::getExistingDirectory(this, "Select Workspace Folder");
            if (!dir.isEmpty()) {
                fileModel->setRootPath(dir);
                treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(dir)));
                QSettings(iniPath, QSettings::IniFormat).setValue("LastFolder", dir);
                QDir tempDir(QDir::tempPath() + "/igi_temp_mef");
                if (tempDir.exists()) tempDir.removeRecursively();
                logMessage("[INFO] Workspace changed. Smart cache cleared.");
            }
        }, QKeySequence("Ctrl+Shift+O"));
        
        fileMenu->addAction(QIcon::fromTheme("document-save"), "&Save", this, [this]() {
            if (viewerEdit->isVisible() && !currentFile.isEmpty() && !viewerEdit->isReadOnly()) {
                if (currentExt == "qvm") {
                    QString tempQscPath = QDir::tempPath() + "/igi_temp.qsc";
                    QFile f(tempQscPath);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        f.write(viewerEdit->toPlainText().toUtf8());
                        f.close();
                        QString cmd = qApp->applicationFilePath();
                        QProcess::execute(cmd, QStringList() << "qsc" << "compile" << tempQscPath << "-o" << currentFile);
                        logMessage("[INFO] Compiled QSC to QVM and saved successfully!");
                        QMessageBox::information(this, "Saved", "Compiled and saved QVM successfully!");
                    }
                } else if (viewModeCombo->currentIndex() == 2) { // Hex View
                    QString text = viewerEdit->toPlainText();
                    QByteArray data;
                    QStringList lines = text.split("\n");
                    for (const QString& line : lines) {
                        if (line.isEmpty() || line.startsWith("...")) continue;
                        int pipe = line.indexOf("|");
                        if (pipe == -1) continue;
                        QString hexPart = line.mid(10, pipe - 10);
                        QStringList bytes = hexPart.split(" ", Qt::SkipEmptyParts);
                        for (const QString& b : bytes) {
                            data.append((char)b.toInt(nullptr, 16));
                        }
                    }
                    QFile f(currentFile);
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(data);
                        logMessage("[INFO] Hex File saved successfully!");
                        QMessageBox::information(this, "Saved", "Hex File saved successfully!");
                    }
                } else {
                    QFile f(currentFile);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        f.write(viewerEdit->toPlainText().toUtf8());
                        logMessage("[INFO] File saved successfully!");
                        QMessageBox::information(this, "Saved", "File saved successfully!");
                    }
                }
            }
        }, QKeySequence::Save);
        
        fileMenu->addSeparator();
        fileMenu->addAction("E&xit", this, &QWidget::close, QKeySequence::Quit);

        QMenu* editMenu = menuBar()->addMenu("&Edit");
        editMenu->addAction("Undo", this, [this](){ viewerEdit->undo(); }, QKeySequence::Undo);
        editMenu->addAction("Redo", this, [this](){ viewerEdit->redo(); }, QKeySequence::Redo);
        editMenu->addSeparator();
        editMenu->addAction("Cut", this, [this](){ viewerEdit->cut(); }, QKeySequence::Cut);
        editMenu->addAction("Copy", this, [this](){ viewerEdit->copy(); }, QKeySequence::Copy);
        editMenu->addAction("Paste", this, [this](){ viewerEdit->paste(); }, QKeySequence::Paste);
        editMenu->addSeparator();
        editMenu->addAction("Find in Text", this, [this](){
            textSearchWidget->setVisible(!textSearchWidget->isVisible());
            if (textSearchWidget->isVisible()) textSearchBox->setFocus();
            else textSearchWidget->hide();
        }, QKeySequence("Ctrl+F"));
        editMenu->addAction("Select All",  this, [this](){ viewerEdit->selectAll(); },   QKeySequence::SelectAll);
        editMenu->addSeparator();
        editMenu->addAction("Find File", this, [this](){
            fileSearchBox->setVisible(!fileSearchBox->isVisible());
            if (fileSearchBox->isVisible()) fileSearchBox->setFocus();
            else { fileSearchBox->clear(); treeView->setFocus(); }
        }, QKeySequence("Ctrl+Shift+F"));
        editMenu->addAction("Replace...", this, [this](){
            // Simple find/replace dialog
            QDialog dlg(this); dlg.setWindowTitle("Find & Replace");
            QVBoxLayout* vl = new QVBoxLayout(&dlg);
            QHBoxLayout* r1 = new QHBoxLayout(); QLineEdit* find = new QLineEdit(); r1->addWidget(new QLabel("Find:")); r1->addWidget(find);
            QHBoxLayout* r2 = new QHBoxLayout(); QLineEdit* repl = new QLineEdit(); r2->addWidget(new QLabel("Replace:")); r2->addWidget(repl);
            QHBoxLayout* r3 = new QHBoxLayout();
            QPushButton* btnReplace = new QPushButton("Replace"); QPushButton* btnAll = new QPushButton("Replace All"); QPushButton* btnClose = new QPushButton("Close");
            r3->addWidget(btnReplace); r3->addWidget(btnAll); r3->addWidget(btnClose);
            vl->addLayout(r1); vl->addLayout(r2); vl->addLayout(r3);
            connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::reject);
            connect(btnReplace, &QPushButton::clicked, [&](){ viewerEdit->find(find->text()); });
            connect(btnAll, &QPushButton::clicked, [&](){
                QString txt = viewerEdit->toPlainText();
                if (!find->text().isEmpty()) { txt.replace(find->text(), repl->text()); viewerEdit->setPlainText(txt); }
            });
            dlg.exec();
        }, QKeySequence("Ctrl+H"));
        // F2 = rename selected file in tree
        QShortcut* f2Rename = new QShortcut(QKeySequence(Qt::Key_F2), this);
        connect(f2Rename, &QShortcut::activated, this, [this](){
            if (treeView) {
                QModelIndex idx = proxyModel->mapToSource(treeView->currentIndex());
                if (idx.isValid()) treeView->edit(proxyModel->mapFromSource(idx));
            }
        });

        QMenu* viewMenu = menuBar()->addMenu("&View");
        viewMenu->addAction("Auto", [this]() { viewModeCombo->setCurrentIndex(0); });
        viewMenu->addAction("Text", [this]() { viewModeCombo->setCurrentIndex(1); });
        viewMenu->addAction("Hex", [this]() { viewModeCombo->setCurrentIndex(2); });
        viewMenu->addAction("Image", [this]() { viewModeCombo->setCurrentIndex(3); });
        viewMenu->addAction("Audio", [this]() { viewModeCombo->setCurrentIndex(4); });
        viewMenu->addAction("Animation", [this]() { viewModeCombo->setCurrentIndex(5); });
        viewMenu->addAction("3D", [this]() { viewModeCombo->setCurrentIndex(6); });
        viewMenu->addSeparator();
        auto applyMenuTheme = [iniPath](const QString& name) {
            QSettings(iniPath, QSettings::IniFormat).setValue("Theme", name);
            if (name == "Light") {
                QPalette p;
                p.setColor(QPalette::Window, QColor(240,240,240));
                p.setColor(QPalette::WindowText, Qt::black);
                p.setColor(QPalette::Base, Qt::white);
                p.setColor(QPalette::AlternateBase, QColor(233,233,233));
                p.setColor(QPalette::ToolTipBase, Qt::white);
                p.setColor(QPalette::ToolTipText, Qt::black);
                p.setColor(QPalette::Text, Qt::black);
                p.setColor(QPalette::Button, QColor(240,240,240));
                p.setColor(QPalette::ButtonText, Qt::black);
                p.setColor(QPalette::BrightText, Qt::red);
                p.setColor(QPalette::Link, QColor(0,0,200));
                p.setColor(QPalette::Highlight, QColor(0,120,215));
                p.setColor(QPalette::HighlightedText, Qt::white);
                qApp->setPalette(p);
                qApp->setStyleSheet(
                    "QTextEdit{background:white;color:black;border:1px solid #ccc;}"
                    "QPlainTextEdit{background:white;color:black;border:1px solid #ccc;}"
                    "QTreeView{background:white;color:black;alternate-background-color:#f0f0f0;}"
                    "QLineEdit{background:white;color:black;border:1px solid #aaa;border-radius:3px;}"
                    "QToolBar{background:#ebebeb;border-bottom:1px solid #ccc;spacing:4px;padding:2px 6px;}"
                    "QMenuBar{background:#f0f0f0;color:black;}"
                    "QMenu{background:#f0f0f0;color:black;}"
                    "QStatusBar{background:#f0f0f0;color:black;}"
                    "QLabel{color:black;}"
                    "QSplitter::handle{background:#ccc;}"
                );
            } else if (name == "Solarized") {
                QPalette p;
                p.setColor(QPalette::Window, QColor(0,43,54));
                p.setColor(QPalette::WindowText, QColor(131,148,150));
                p.setColor(QPalette::Base, QColor(7,54,66));
                p.setColor(QPalette::AlternateBase, QColor(0,43,54));
                p.setColor(QPalette::Text, QColor(131,148,150));
                p.setColor(QPalette::Button, QColor(0,43,54));
                p.setColor(QPalette::ButtonText, QColor(131,148,150));
                p.setColor(QPalette::Highlight, QColor(38,139,210));
                p.setColor(QPalette::HighlightedText, Qt::white);
                qApp->setPalette(p);
                qApp->setStyleSheet("");
            } else if (name == "Military") {
                QPalette p;
                p.setColor(QPalette::Window, QColor(30,40,20));
                p.setColor(QPalette::WindowText, QColor(120,200,60));
                p.setColor(QPalette::Base, QColor(15,25,10));
                p.setColor(QPalette::AlternateBase, QColor(30,40,20));
                p.setColor(QPalette::Text, QColor(120,200,60));
                p.setColor(QPalette::Button, QColor(30,40,20));
                p.setColor(QPalette::ButtonText, QColor(120,200,60));
                p.setColor(QPalette::Highlight, QColor(80,140,30));
                p.setColor(QPalette::HighlightedText, Qt::black);
                qApp->setPalette(p);
                qApp->setStyleSheet("");
            } else {
                // Dark
                QPalette p;
                p.setColor(QPalette::Window, QColor(53,53,53));
                p.setColor(QPalette::WindowText, Qt::white);
                p.setColor(QPalette::Base, QColor(25,25,25));
                p.setColor(QPalette::AlternateBase, QColor(53,53,53));
                p.setColor(QPalette::ToolTipBase, Qt::white);
                p.setColor(QPalette::ToolTipText, Qt::white);
                p.setColor(QPalette::Text, Qt::white);
                p.setColor(QPalette::Button, QColor(53,53,53));
                p.setColor(QPalette::ButtonText, Qt::white);
                p.setColor(QPalette::BrightText, Qt::red);
                p.setColor(QPalette::Link, QColor(42,130,218));
                p.setColor(QPalette::Highlight, QColor(42,130,218));
                p.setColor(QPalette::HighlightedText, Qt::black);
                qApp->setPalette(p);
                qApp->setStyleSheet("");
            }
        };
        // Themes go in a submenu so the parent View menu stays short.
        // The user complained about the old flat "Light Theme / Dark
        // Theme / Solarized Theme / Military Theme" entries filling
        // the View menu — they wanted a "Themes" cascade instead.
        QMenu* themesMenu = viewMenu->addMenu("Themes");
        themesMenu->addAction("Light Theme",    [applyMenuTheme]() { applyMenuTheme("Light"); });
        themesMenu->addAction("Dark Theme",     [applyMenuTheme]() { applyMenuTheme("Dark"); });
        themesMenu->addAction("Solarized Theme",[applyMenuTheme]() { applyMenuTheme("Solarized"); });
        themesMenu->addAction("Military Theme", [applyMenuTheme]() { applyMenuTheme("Military"); });


        QSettings settings(iniPath, QSettings::IniFormat);
        globalLevelMtpPath = settings.value("LevelMTP", "").toString();
        globalLevelDatPath = settings.value("LevelDAT", "").toString();
        globalTextureDir = settings.value("TextureDir", "").toString();
        globalCacheDir = settings.value("CacheDir", QDir::tempPath() + "/igi_temp_mef").toString();
        globalAutoSaveRes = settings.value("AutoSaveRes", false).toBool();
        QString logLevel = settings.value("LOGS_LEVEL", "INFO").toString();

        QMenu* settingsMenu = menuBar()->addMenu("&Settings");

        // ── Animation submenu ───────────────────────────────────────────────
        // The new "Animation" mode is gated by this menu: the Mode
        // combo only shows the "Animation" entry when at least one of
        // the four prerequisites is true.  All entries persist into
        // igi1conv.ini via the standard QSettings flow.
        QMenu* animSettingsMenu = settingsMenu->addMenu("Animation");
        globalAnimationModeEnabled = settings.value("AnimationModeEnabled", false).toBool();
        globalObjectsQscPath       = settings.value("ObjectsQscPath", "").toString();
        globalAnimsSourceDir       = settings.value("AnimsSourceDir", "").toString();
        globalModelsDir            = settings.value("ModelsDir", "").toString();
        globalAnimsCacheDir        = settings.value("AnimsCacheDir",
            globalCacheDir + "/animation_anims").toString();

        QAction* animModeAction = animSettingsMenu->addAction("Enable Animation Mode");
        animModeAction->setCheckable(true);
        animModeAction->setChecked(globalAnimationModeEnabled);
        connect(animModeAction, &QAction::toggled, this,
            [this, iniPath](bool checked) {
                globalAnimationModeEnabled = checked;
                QSettings(iniPath, QSettings::IniFormat).setValue("AnimationModeEnabled", checked);
                if (viewModeCombo) {
                    // Re-sync the mode combo so "Animation" appears /
                    // disappears in lock-step with the toggle.
                    int curIdx = viewModeCombo->currentIndex();
                    rebuildModeCombo();
                    int newIdx = checked ? 6 : qMin(curIdx, 5);
                    viewModeCombo->setCurrentIndex(newIdx);
                }
                logMessage(QString("[INFO] Animation mode is now %1")
                    .arg(checked ? "ENABLED" : "DISABLED"));
                // When the user enables the mode, eagerly auto-detect
                // the objects.qsc / ANIMS / models folders from the
                // currently configured LevelPath so the dropdown
                // populates immediately and the Play button lights up
                // without further user action.
                if (checked) {
                    autoDetectAnimationFolders();
                    loadAnimationSetFromQsc();
                }
            });

        animSettingsMenu->addSeparator();

        animSettingsMenu->addAction("Set Objects.qsc...", this, [this, iniPath]() {
            QString path = QFileDialog::getOpenFileName(
                this, "Select decompiled objects.qsc (run 'qvm decompile' first)",
                globalObjectsQscPath.isEmpty()
                    ? QDir::homePath()
                    : QFileInfo(globalObjectsQscPath).absolutePath(),
                "QScript (*.qsc);;All files (*)");
            if (path.isEmpty()) return;
            globalObjectsQscPath = path;
            QSettings(iniPath, QSettings::IniFormat).setValue("ObjectsQscPath", path);
            logMessage("[INFO] Objects.qsc set: " + path);
            // Eagerly parse so the dropdown is populated the moment
            // the user enables the mode.
            loadAnimationSetFromQsc();
        });

        animSettingsMenu->addAction("Set ANIMS Source Folder...", this, [this, iniPath]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, "Select COMMON/ANIMS folder (the game's IFF animation source)",
                globalAnimsSourceDir.isEmpty() ? QDir::homePath() : globalAnimsSourceDir);
            if (dir.isEmpty()) return;
            globalAnimsSourceDir = dir;
            QSettings(iniPath, QSettings::IniFormat).setValue("AnimsSourceDir", dir);
            logMessage("[INFO] ANIMS source folder set: " + dir);
        });

        animSettingsMenu->addAction("Set LEVEL Models Folder...", this, [this, iniPath]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, "Select LEVEL1/models folder",
                globalModelsDir.isEmpty() ? QDir::homePath() : globalModelsDir);
            if (dir.isEmpty()) return;
            globalModelsDir = dir;
            QSettings(iniPath, QSettings::IniFormat).setValue("ModelsDir", dir);
            logMessage("[INFO] Models folder set: " + dir);
        });

        animSettingsMenu->addSeparator();

        animSettingsMenu->addAction("Pre-Extract All ANIMS to Cache", this, [this]() {
            preExtractAllAnims();
        });

        animSettingsMenu->addAction("Clear Animation Cache", this, [this]() {
            if (globalAnimsCacheDir.isEmpty()) return;
            QDir d(globalAnimsCacheDir);
            if (!d.exists()) {
                logMessage("[INFO] Animation cache does not exist: " + globalAnimsCacheDir);
                return;
            }
            int n = d.removeRecursively() ? 1 : 0;
            logMessage(QString("[INFO] Animation cache cleared (%1)").arg(globalAnimsCacheDir));
        });

        animSettingsMenu->addAction("Reload Animation Set", this, [this]() {
            loadAnimationSetFromQsc();
        });

        // ── Auto Save on RES (unchanged) ─────────────────────────────────────
        QAction* autoSaveResAction = settingsMenu->addAction("Auto Save on RES");
        autoSaveResAction->setCheckable(true);
        autoSaveResAction->setChecked(globalAutoSaveRes);
        connect(autoSaveResAction, &QAction::toggled, this, [this, iniPath](bool checked) {
            globalAutoSaveRes = checked;
            QSettings(iniPath, QSettings::IniFormat).setValue("AutoSaveRes", checked);
            logMessage(QString("[INFO] Auto Save on RES is now %1").arg(checked ? "ON" : "OFF"));
        });

        QMenu* levelMenu = settingsMenu->addMenu("&Level");
        levelMenu->addAction("Set Level...", this, [this, iniPath]() {
            QString levelDir = QFileDialog::getExistingDirectory(this, "Select Level Folder (e.g. LEVEL8)", globalLevelDatPath.isEmpty() ? QDir::homePath() : QFileInfo(globalLevelDatPath).absolutePath());
            if (levelDir.isEmpty()) return;
            
            // Auto-resolve DAT, MTP, Textures from the level folder
            QDir dir(levelDir);
            QString levelName = dir.dirName(); // e.g. "LEVEL8"
            
            // Find .DAT
            QStringList dats = dir.entryList(QStringList() << "*.dat" << "*.DAT", QDir::Files);
            if (!dats.isEmpty()) {
                globalLevelDatPath = levelDir + "/" + dats.first();
                QSettings(iniPath, QSettings::IniFormat).setValue("LevelDAT", globalLevelDatPath);
                logMessage("[INFO] Level DAT auto-set: " + globalLevelDatPath);
            }
            
            // Find .MTP
            QStringList mtps = dir.entryList(QStringList() << "*.mtp" << "*.MTP", QDir::Files);
            if (!mtps.isEmpty()) {
                globalLevelMtpPath = levelDir + "/" + mtps.first();
                QSettings(iniPath, QSettings::IniFormat).setValue("LevelMTP", globalLevelMtpPath);
                logMessage("[INFO] Level MTP auto-set: " + globalLevelMtpPath);
            }
            
            // Find TEXTURES subfolder
            QStringList textureDirs = {"TEXTURES", "textures", "Textures"};
            for (const QString& td : textureDirs) {
                if (QDir(levelDir + "/" + td).exists()) {
                    globalTextureDir = levelDir + "/" + td;
                    break;
                }
            }
            // If not found, check for .RES file and auto-extract
            if (globalTextureDir.isEmpty()) {
                QStringList resList = dir.entryList(QStringList() << "*.res" << "*.RES", QDir::Files);
                for (const QString& resName : resList) {
                    if (resName.contains(levelName, Qt::CaseInsensitive) || resName.contains("TEXTURES", Qt::CaseInsensitive)) {
                        globalTextureDir = levelDir + "/TEXTURES";
                        QDir().mkpath(globalTextureDir);
                        QString resPath = levelDir + "/" + resName;
                        logMessage("[INFO] Extracting " + resName + " -> " + globalTextureDir);
                        QProcess::execute(qApp->applicationFilePath(), QStringList() << "res" << "extract" << resPath << "-o" << globalTextureDir);
                        break;
                    }
                }
            }
            if (!globalTextureDir.isEmpty()) {
                QSettings(iniPath, QSettings::IniFormat).setValue("TextureDir", globalTextureDir);
                logMessage("[INFO] Texture Dir auto-set: " + globalTextureDir);
                
                // Extract any .res files inside texture dir
                QDir texDir(globalTextureDir);
                for (const QString& res : texDir.entryList(QStringList() << "*.res" << "*.RES", QDir::Files)) {
                    logMessage("[INFO] Extracting " + res + " from textures dir...");
                    QProcess::execute(qApp->applicationFilePath(), QStringList() << "res" << "extract" << texDir.absoluteFilePath(res) << "-o" << globalTextureDir);
                }
            }
            QSettings(iniPath, QSettings::IniFormat).setValue("LevelPath", levelDir);
            
            // ── Auto-setup Animation mode paths ──────────────────────────────
            // When the user picks a level, automatically:
            //   1. Find & decompile objects.qvm → objects.qsc
            //   2. Find the ANIMS folder (walk up to common/ANIMS)
            //   3. Find the models folder (level/models or level/models/unpacked)
            //   4. Pre-extract all ANIMS to cache
            //   5. Enable Animation mode + load the set
            // All paths are persisted to igi1conv.ini.
            logMessage("[INFO] Auto-setting up Animation mode paths...");
            QString animsDir, modelsDir, objectsQsc;

            // 1. Find objects.qvm in this level folder (or sibling folders)
            QStringList qvmCandidates;
            {
                QDir d(levelDir);
                // Check this level folder
                QStringList qvms = d.entryList(QStringList() << "objects.qvm" << "OBJECTS.QVM", QDir::Files);
                for (const auto& q : qvms) qvmCandidates << (levelDir + "/" + q);
                // Check sibling level folders (objects.qvm might be in level1 only)
                if (d.cdUp()) {
                    QDirIterator it(d.absolutePath(),
                                    QStringList() << "objects.qvm" << "OBJECTS.QVM",
                                    QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        QString p = it.next();
                        if (!qvmCandidates.contains(p)) qvmCandidates << p;
                    }
                }
            }
            // Also check if objects.qsc already exists
            for (const auto& q : qvmCandidates) {
                QString qscPath = QFileInfo(q).absolutePath() + "/objects.qsc";
                if (QFile::exists(qscPath)) {
                    objectsQsc = qscPath;
                    logMessage("[INFO] Animation: found existing objects.qsc: " + qscPath);
                    break;
                }
            }
            if (objectsQsc.isEmpty() && !qvmCandidates.isEmpty()) {
                // Decompile objects.qvm → objects.qsc
                QString qvmPath = qvmCandidates.first();
                objectsQsc = QFileInfo(qvmPath).absolutePath() + "/objects.qsc";
                logMessage("[INFO] Animation: decompiling " + qvmPath + " -> " + objectsQsc);
                QProcess::execute(qApp->applicationFilePath(),
                    QStringList() << "qvm" << "decompile" << qvmPath << "-o" << objectsQsc);
            }
            if (!objectsQsc.isEmpty() && QFile::exists(objectsQsc)) {
                globalObjectsQscPath = objectsQsc;
                QSettings(iniPath, QSettings::IniFormat).setValue("ObjectsQscPath", objectsQsc);
                logMessage("[INFO] Animation: objects.qsc set: " + objectsQsc);
            }

            // 2. Find ANIMS folder (walk up to find common/ANIMS)
            {
                QDir d(levelDir);
                for (int i = 0; i < 6 && d.cdUp(); ++i) {
                    QStringList tryNames = {"common/ANIMS", "Common/ANIMS", "COMMON/ANIMS", "common/anims", "ANIMS"};
                    for (const QString& n : tryNames) {
                        QString c = d.absoluteFilePath(n);
                        if (QDir(c).exists()) { animsDir = c; break; }
                    }
                    if (!animsDir.isEmpty()) break;
                }
            }
            if (!animsDir.isEmpty()) {
                globalAnimsSourceDir = animsDir;
                QSettings(iniPath, QSettings::IniFormat).setValue("AnimsSourceDir", animsDir);
                logMessage("[INFO] Animation: ANIMS folder set: " + animsDir);
            }

            // 3. Find models folder
            {
                QStringList modelCands;
                modelCands << (levelDir + "/models")
                           << (levelDir + "/MODELS")
                           << (levelDir + "/Models");
                // Also check for unpacked models
                QDir md(levelDir + "/models");
                if (md.exists()) {
                    QStringList subs = md.entryList(QStringList() << "*unpacked*", QDir::Dirs);
                    for (const auto& s : subs) modelCands << (levelDir + "/models/" + s);
                }
                for (const auto& c : modelCands) {
                    if (QDir(c).exists() && !QDir(c).entryList(QStringList() << "*.mef" << "*.MEF", QDir::Files).isEmpty()) {
                        modelsDir = c;
                        break;
                    }
                }
                // If no .mef files found directly, check for .res to unpack
                if (modelsDir.isEmpty()) {
                    QString resPath = levelDir + "/models/" + levelName + ".res";
                    if (!QFile::exists(resPath)) {
                        QStringList resList = QDir(levelDir + "/models").entryList(QStringList() << "*.res" << "*.RES", QDir::Files);
                        if (!resList.isEmpty()) resPath = levelDir + "/models/" + resList.first();
                    }
                    if (QFile::exists(resPath)) {
                        modelsDir = levelDir + "/models/" + levelName + "_unpacked";
                        QDir().mkpath(modelsDir);
                        logMessage("[INFO] Animation: unpacking models from " + resPath + " -> " + modelsDir);
                        QProcess::execute(qApp->applicationFilePath(),
                            QStringList() << "res" << "unpack" << resPath << modelsDir);
                    }
                }
            }
            if (!modelsDir.isEmpty()) {
                globalModelsDir = modelsDir;
                QSettings(iniPath, QSettings::IniFormat).setValue("ModelsDir", modelsDir);
                logMessage("[INFO] Animation: models folder set: " + modelsDir);
            }

            // 4. Set cache dir for ANIMS
            globalAnimsCacheDir = globalCacheDir + "/animation_anims";
            QSettings(iniPath, QSettings::IniFormat).setValue("AnimsCacheDir", globalAnimsCacheDir);

            // 5. Pre-extract all ANIMS
            if (!animsDir.isEmpty()) {
                QDir().mkpath(globalAnimsCacheDir);
                QDir srcDir(animsDir);
                QStringList filters; filters << "*.IFF" << "*.iff" << "*.BFF" << "*.bff";
                int copied = 0;
                for (const QFileInfo& fi : srcDir.entryInfoList(filters, QDir::Files)) {
                    QString dst = globalAnimsCacheDir + "/" + fi.fileName();
                    if (!QFile::exists(dst)) {
                        if (QFile::copy(fi.absoluteFilePath(), dst)) ++copied;
                    }
                }
                logMessage(QString("[INFO] Animation: pre-extracted %1 ANIMS files to cache").arg(copied));
            }

            // 6. Enable Animation mode + load set
            if (!objectsQsc.isEmpty()) {
                globalAnimationModeEnabled = true;
                QSettings(iniPath, QSettings::IniFormat).setValue("AnimationModeEnabled", true);
                rebuildModeCombo();
                loadAnimationSetFromQsc();
                logMessage("[INFO] Animation mode auto-enabled with all paths configured.");
            }

            QMessageBox::information(this, "Level Set",
                QString("Level: %1\nDAT: %2\nMTP: %3\nTextures: %4\n\nAnimation Mode:\nobjects.qsc: %5\nANIMS: %6\nModels: %7")
                .arg(levelDir, globalLevelDatPath, globalLevelMtpPath, globalTextureDir,
                     objectsQsc, animsDir, modelsDir));
        });

        settingsMenu->addAction("Cache Folder...", this, [this, iniPath]() {
            QString newCache = QFileDialog::getExistingDirectory(this, "Select Cache Folder (audio, textures, models, animations)", globalCacheDir);
            if (!newCache.isEmpty()) {
                globalCacheDir = newCache;
                modelViewer->cacheDir = globalCacheDir;
                QSettings(iniPath, QSettings::IniFormat).setValue("CacheDir", globalCacheDir);
                logMessage("[INFO] Cache Folder set to: " + globalCacheDir);
            }
        });

        auto doClearCache = [this]() {
            QDir dir(globalCacheDir);
            if (!dir.exists()) {
                logMessage("[INFO] Cache folder does not exist: " + globalCacheDir);
                return;
            }
            int files = 0;
            qint64 bytes = 0;
            for (const QFileInfo& fi : QDir(globalCacheDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
                { files++; bytes += fi.size(); }
            // Walk subdirs too
            QDirIterator it(globalCacheDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) { it.next(); files++; bytes += it.fileInfo().size(); }
            for (const QString& entry : dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                QFileInfo fi(globalCacheDir + "/" + entry);
                if (fi.isDir()) QDir(fi.absoluteFilePath()).removeRecursively();
                else            QFile::remove(fi.absoluteFilePath());
            }
            logMessage(QString("[INFO] Cache cleared: %1 files removed (%2 KB freed) — %3")
                        .arg(files).arg(bytes / 1024).arg(globalCacheDir));
        };

        settingsMenu->addAction("Clear Cache", this, doClearCache);

        // Small buttons in the menu-bar corner: Refresh (file tree) +
        // Clear Cache (temporary cache folder).  Both sit at the
        // top-right of the main window so they're always reachable
        // regardless of the active view.
        QWidget* cornerBtns = new QWidget(menuBar());
        QHBoxLayout* cornerLayout = new QHBoxLayout(cornerBtns);
        cornerLayout->setContentsMargins(0, 0, 4, 0);
        cornerLayout->setSpacing(4);
        auto mkCornerBtn = [](const QString& text, const QString& tip) {
            QPushButton* b = new QPushButton(text);
            b->setFlat(true);
            b->setToolTip(tip);
            b->setStyleSheet(
                "QPushButton{color:#ccc;background:transparent;border:1px solid #555;"
                "border-radius:3px;padding:2px 8px;font-size:11px;}"
                "QPushButton:hover{background:#3a3a3a;border-color:#888;}"
                "QPushButton:pressed{background:#222;}");
            return b;
        };
        QPushButton* refreshBtn = mkCornerBtn("\xf0\x9f\x94\x84 Refresh",
            "Refresh the file tree from disk");
        QPushButton* clearCacheBtn = mkCornerBtn("\xf0\x9f\x97\x91 Clear Cache",
            "Clear temporary cache folder");
        cornerLayout->addWidget(refreshBtn);
        cornerLayout->addWidget(clearCacheBtn);
        connect(refreshBtn, &QPushButton::clicked, this, [this]() {
            // Refresh the file model from disk so newly-added / deleted
            // files in the current folder appear in the tree.
            QString currentRoot = fileModel->rootPath();
            if (currentRoot.isEmpty()) currentRoot = QDir::currentPath();
            fileModel->setRootPath("");
            fileModel->setRootPath(currentRoot);
            treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(currentRoot)));
            logMessage("[INFO] Refreshed file list from " + currentRoot);
        });
        connect(clearCacheBtn, &QPushButton::clicked, this, doClearCache);
        menuBar()->setCornerWidget(cornerBtns, Qt::TopRightCorner);

        QMenu* logsMenu = settingsMenu->addMenu("&Logs");
        logsMenu->addAction("Enable Logs", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_ENABLED", true);
            logMessage("[INFO] Logs Enabled");
        });
        logsMenu->addAction("Disable Logs", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_ENABLED", false);
            logMessage("[INFO] Logs Disabled");
        });

        QMenu* logsLevelMenu = logsMenu->addMenu("Logs Level");
        logsLevelMenu->addAction("INFO", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_LEVEL", "INFO");
            logMessage("[INFO] LOGS_LEVEL set to INFO", LOG_INFO);
        });
        logsLevelMenu->addAction("DEBUG", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_LEVEL", "DEBUG");
            logMessage("[INFO] LOGS_LEVEL set to DEBUG", LOG_INFO);
        });
        logsLevelMenu->addAction("ERROR", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_LEVEL", "ERROR");
            logMessage("[INFO] LOGS_LEVEL set to ERROR", LOG_INFO);
        });
        logsLevelMenu->addAction("VERBOSE", this, [this, iniPath]() {
            QSettings(iniPath, QSettings::IniFormat).setValue("LOGS_LEVEL", "VERBOSE");
            logMessage("[INFO] LOGS_LEVEL set to VERBOSE", LOG_INFO);
        });

        QMenu* helpMenu = menuBar()->addMenu("&Help");
        helpMenu->addAction("About", this, [this]() {
            QMessageBox::about(this, "About", "IGI Game Convertor\nVersion 1.11.0\nAuthor: HeavenHM\nDeveloped in C++ with Qt5/Qt6.\nAdvanced Edition with MEF Native Viewer and full CLI integration.");
        });

        QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
        setCentralWidget(splitter);

        // Left side: File browser
        QWidget* leftWidget = new QWidget(splitter);
        QVBoxLayout* leftLayout = new QVBoxLayout(leftWidget);
        leftLayout->setContentsMargins(0,0,0,0);
        fileSearchBox = new QLineEdit();
        fileSearchBox->setPlaceholderText("Search file by name...");
        fileSearchBox->hide();
        leftLayout->addWidget(fileSearchBox);
        proxyModel->setRecursiveFilteringEnabled(true);

        connect(fileSearchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
            // First check if 'text' matches a model name to resolve its ID prefix
            static QJsonArray s_modelsArr;
            static bool s_loaded = false;
            if (!s_loaded) {
                s_loaded = true;
                QFile f(QCoreApplication::applicationDirPath() + "/IGIModels.json");
                if (f.open(QIODevice::ReadOnly)) {
                    s_modelsArr = QJsonDocument::fromJson(f.readAll()).array();
                }
            }
            QString exactMatchId;
            QString q = text.trimmed().toLower();
            for (const QJsonValue& v : s_modelsArr) {
                QJsonObject obj = v.toObject();
                QString id   = obj["ModelId"].toString();
                QString name = obj["ModelName"].toString();
                if (id.toLower() == q || name.toLower() == q) { exactMatchId = id; break; }
            }
            
            if (!exactMatchId.isEmpty()) {
                QString basePrefix = exactMatchId.split('_').first();
                proxyModel->setFilterWildcard("*" + basePrefix + "*");
            } else {
                proxyModel->setFilterWildcard("*" + text + "*");
            }
            
            if (this->treeView) {
                QString currentRoot = fileModel->rootPath();
                if (currentRoot.isEmpty()) currentRoot = QDir::currentPath();
                this->treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(currentRoot)));
            }
        });
        treeView = new QTreeView(leftWidget);
        treeView->setModel(proxyModel);
        treeView->setRootIndex(proxyModel->mapFromSource(fileModel->index(lastFolder)));
        treeView->setColumnWidth(0, 250);
        treeView->setSortingEnabled(true);
        treeView->sortByColumn(0, Qt::AscendingOrder);
        treeView->setContextMenuPolicy(Qt::CustomContextMenu);
        treeView->setSelectionMode(QAbstractItemView::ExtendedSelection); // Multi-selection
        treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
        treeView->setDragEnabled(true);
        treeView->setAcceptDrops(true);
        treeView->setDropIndicatorShown(true);
        treeView->setDragDropMode(QAbstractItemView::DragDrop);
        treeView->setDefaultDropAction(Qt::CopyAction);

        // Refresh and Clear Cache buttons live in the menu-bar's
        // top-right corner (cornerBtns widget created above), not in
        // a per-tree toolbar, so the file-tree area is clean.
        leftLayout->addWidget(treeView);

        connect(treeView, &QTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);

        // Right side: Viewer and Controls
        QWidget* rightWidget = new QWidget(splitter);
        QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);

        textSearchWidget = new QWidget();
        QHBoxLayout* textSearchLayout = new QHBoxLayout(textSearchWidget);
        textSearchLayout->setContentsMargins(0,0,0,0);
        textSearchBox = new QLineEdit();
        textSearchBox->setPlaceholderText("Find...");
        textSearchLayout->addWidget(textSearchBox);
        
        QLineEdit* replaceBox = new QLineEdit();
        replaceBox->setPlaceholderText("Replace with...");
        textSearchLayout->addWidget(replaceBox);
        
        QPushButton* btnReplace = new QPushButton("Replace");
        textSearchLayout->addWidget(btnReplace);
        QPushButton* btnReplaceAll = new QPushButton("Replace All");
        textSearchLayout->addWidget(btnReplaceAll);

        textSearchWidget->hide();
        rightLayout->addWidget(textSearchWidget);
        
        connect(btnReplace, &QPushButton::clicked, this, [this, replaceBox]() {
            if (viewerEdit->textCursor().hasSelection()) {
                viewerEdit->textCursor().insertText(replaceBox->text());
            }
            viewerEdit->find(textSearchBox->text());
        });
        
        connect(btnReplaceAll, &QPushButton::clicked, this, [this, replaceBox]() {
            QTextCursor cursor = viewerEdit->textCursor();
            cursor.beginEditBlock();
            cursor.movePosition(QTextCursor::Start);
            viewerEdit->setTextCursor(cursor);
            while (viewerEdit->find(textSearchBox->text())) {
                viewerEdit->textCursor().insertText(replaceBox->text());
            }
            cursor.endEditBlock();
        });

        connect(textSearchBox, &QLineEdit::textChanged, this, [this](const QString& text) {
            viewerEdit->moveCursor(QTextCursor::Start);
            viewerEdit->find(textSearchBox->text());
        });
        
        connect(textSearchBox, &QLineEdit::returnPressed, this, [this]() {
            viewerEdit->find(textSearchBox->text());
        });
        
        QShortcut *renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), treeView);
        connect(renameShortcut, &QShortcut::activated, this, [this]() {
            QModelIndex index = treeView->currentIndex();
            QModelIndex srcIndex = proxyModel->mapToSource(index);
            if (srcIndex.isValid()) {
                QString path = fileModel->filePath(srcIndex);
                bool ok;
                QString newName = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
                if (ok && !newName.isEmpty()) {
                    QString newPath = QFileInfo(path).absolutePath() + "/" + newName;
                    if (QFileInfo(path).isDir()) QDir().rename(path, newPath);
                    else QFile::rename(path, newPath);
                }
            }
        });

        // ── Row 1: Mode selector (always on its own line at the top) ────────
        QHBoxLayout* viewModeLayout = new QHBoxLayout();
        viewModeLayout->setContentsMargins(0, 0, 0, 0);
        viewModeLayout->setSpacing(6);
        viewModeLayout->addWidget(new QLabel("Mode:"));
        viewModeCombo = new QComboBox();
        rebuildModeCombo(); // populates the entries; "Animation" only if enabled
        viewModeLayout->addWidget(viewModeCombo);
        viewModeLayout->addStretch();
        rightLayout->addLayout(viewModeLayout);

        // ── Row 2: Unified viewer toolbar (NEW LINE below "Mode: ...") ──────
        // Every per-mode toolbar (graph buttons for graph.dat, IFF media
        // bar for .iff, image editor buttons for .tex/.png/.tga) lives
        // in this single row so the buttons sit at the same screen
        // position regardless of the active view mode.  Each sub-widget
        // is hidden by default and shown only when its matching file
        // type is loaded.
        QWidget* viewerToolbarRow = new QWidget();
        viewerToolbarRow->setObjectName("viewerToolbarRow");
        viewerToolbarRow->setStyleSheet(
            "QWidget#viewerToolbarRow { background:#2a2a2a; border-bottom:1px solid #444; }"
            "QPushButton { background:#3a3a3a; color:#ddd; border:1px solid #555;"
            "  border-radius:3px; padding:3px 10px; font-size:12px; min-width:30px; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton:checked { background:#0066aa; border-color:#0088cc; }"
            "QCheckBox { color:#ddd; font-family:Consolas; font-size:11px; spacing:4px; }"
            "QCheckBox::indicator { width:14px; height:14px; }"
            "QLabel { color:#cfd; font-family:Consolas; font-size:11px;"
            "  background:#333; border:1px solid #555; border-radius:3px;"
            "  padding:2px 8px; }"
        );
        QHBoxLayout* viewerToolbarLayout = new QHBoxLayout(viewerToolbarRow);
        viewerToolbarLayout->setContentsMargins(6, 4, 6, 4);
        viewerToolbarLayout->setSpacing(6);
        rightLayout->addWidget(viewerToolbarRow);

        // ── 3D Graph Buttons (added to the unified viewer toolbar row) ──────
        // Hidden by default; shown when a graph*.dat is loaded via the
        // onGraphLoaded callback below.
        graphToolbar = new QWidget();
        graphToolbar->setObjectName("graphToolbarInline");
        graphToolbar->setStyleSheet("background:transparent;");
        QHBoxLayout* graphRow = new QHBoxLayout(graphToolbar);
        graphRow->setContentsMargins(0, 0, 0, 0);
        graphRow->setSpacing(6);

        btnGraphNodePlus  = new QPushButton("+ Node");
        btnGraphNodePlus->setToolTip("Increase node size");
        btnGraphNodeMinus = new QPushButton("- Node");
        btnGraphNodeMinus->setToolTip("Decrease node size");
        chkGraphNodes     = new QCheckBox("Nodes");
        chkGraphNodes->setChecked(true);
        chkGraphNodes->setToolTip("Toggle node rendering");
        chkGraphLinks     = new QCheckBox("Links");
        chkGraphLinks->setChecked(true);
        chkGraphLinks->setToolTip("Toggle link rendering");
        lblGraphTotalNodes = new QLabel("Nodes: 0");
        lblGraphTotalNodes->setToolTip("Number of nodes in the current graph");
        lblGraphTotalLinks = new QLabel("Links: 0");
        lblGraphTotalLinks->setToolTip("Number of links (edges) in the current graph");
        btnGraphReset     = new QPushButton("Reset");
        btnGraphReset->setToolTip("Reset node scale to default");

        graphRow->addWidget(btnGraphNodePlus);
        graphRow->addWidget(btnGraphNodeMinus);
        graphRow->addSpacing(8);
        graphRow->addWidget(chkGraphNodes);
        graphRow->addWidget(chkGraphLinks);
        graphRow->addSpacing(8);
        graphRow->addWidget(lblGraphTotalNodes);
        graphRow->addWidget(lblGraphTotalLinks);
        graphRow->addSpacing(8);
        graphRow->addWidget(btnGraphReset);

        graphToolbar->hide();
        viewerToolbarLayout->addWidget(graphToolbar);

        viewerEdit = new CodeEditor(this);
        viewerEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        viewerEdit->hide();

        // ── Text editor toolbar ──────────────────────────────────────────────

        imageEditor = new ImageEditor(this);
        imageEditor->hide();
        // Reparent the ImageEditor's internal toolbar into the unified
        // viewer toolbar row (Row 2) so the image buttons (Draw / Erase
        // / Color / Fit / Zoom / Reset / Save) sit at the same screen
        // position as the 3D graph buttons and the IFF media bar.
        if (imageEditor->toolsWidget) {
            imageEditor->toolsWidget->setParent(viewerToolbarRow);
            imageEditor->toolsWidget->setStyleSheet("background:transparent;");
            imageEditor->toolsWidget->hide();
            viewerToolbarLayout->addWidget(imageEditor->toolsWidget);
        }

        modelViewer = new ModelViewer();
        modelViewer->cacheDir = globalCacheDir;
        // Wire ModelViewer's graph-load callback to MainWindow so
        // the 3D-view graph toolbar (Total Nodes / Total Links /
        // Nodes / Links toggles) is updated every time a new
        // graph.dat is opened.
        modelViewer->onGraphLoaded = [this](int nodeCount, int linkCount) {
            if (lblGraphTotalNodes) lblGraphTotalNodes->setText(
                QString("Total Nodes: %1").arg(nodeCount));
            if (lblGraphTotalLinks) lblGraphTotalLinks->setText(
                QString("Total Links: %1").arg(linkCount));
            if (chkGraphNodes)  chkGraphNodes->setChecked(modelViewer->showGraphNodes);
            if (chkGraphLinks)  chkGraphLinks->setChecked(modelViewer->showGraphLinks);
            if (graphToolbar) {
                if (nodeCount > 0) graphToolbar->show();
                else graphToolbar->hide();
            }
        };

        rightLayout->addWidget(viewerEdit, 3);
        rightLayout->addWidget(imageEditor, 3);
        rightLayout->addWidget(modelViewer, 3);

        // Graph hex editor: read-only hex viewer with graph-specific
        // info panel (Signature, Data-Type, Node #, Graph Data) used
        // when the user opens a graph*.dat file in Hex mode.  Stays
        // hidden unless a graph file is loaded.
        graphHexEditor = new GraphHexEditor();
        graphHexEditor->hide();
        rightLayout->addWidget(graphHexEditor, 3);

        // Now that modelViewer exists, wire the graph toolbar buttons
        // to the modelViewer's state.  The toolbar widgets themselves
        // were created above (and added to rightLayout just below the
        // Mode row, so they sit on top of the 3D viewer), but the
        // lambdas can only be connected once modelViewer is alive.
        // IMPORTANT: graphNodeScale and showGraphNodes/showGraphLinks
        // are baked into the vertex buffers by generateGraphGeometry,
        // so a button click must regenerate the geometry AND call
        // update() for the change to be visible.
        if (btnGraphNodePlus) {
            connect(btnGraphNodePlus, &QPushButton::clicked, this, [this]() {
                if (!modelViewer) return;
                modelViewer->graphNodeScale *= 1.2f;
                modelViewer->regenerateGraphGeometry();
                modelViewer->update();
            });
        }
        if (btnGraphNodeMinus) {
            connect(btnGraphNodeMinus, &QPushButton::clicked, this, [this]() {
                if (!modelViewer) return;
                modelViewer->graphNodeScale =
                    std::max(0.05f, modelViewer->graphNodeScale / 1.2f);
                modelViewer->regenerateGraphGeometry();
                modelViewer->update();
            });
        }
        if (btnGraphReset) {
            connect(btnGraphReset, &QPushButton::clicked, this, [this]() {
                if (!modelViewer) return;
                modelViewer->graphNodeScale = 1.0f;
                modelViewer->regenerateGraphGeometry();
                modelViewer->update();
            });
        }
        if (chkGraphNodes) {
            connect(chkGraphNodes, &QCheckBox::toggled, this, [this](bool on) {
                if (!modelViewer) return;
                modelViewer->showGraphNodes = on;
                modelViewer->regenerateGraphGeometry();
                modelViewer->update();
            });
        }
        if (chkGraphLinks) {
            connect(chkGraphLinks, &QCheckBox::toggled, this, [this](bool on) {
                if (!modelViewer) return;
                modelViewer->showGraphLinks = on;
                modelViewer->regenerateGraphGeometry();
                modelViewer->update();
            });
        }

        // ── IFF Media Player Bar ──────────────────────────────────────────────
        iffMediaBar = new QWidget();
        iffMediaBar->setObjectName("iffMediaBar");
        iffMediaBar->setStyleSheet(
            "QWidget#iffMediaBar { background:#1a1a2e; border-top:1px solid #444; padding:4px; }"
            "QPushButton { background:#252545; color:#e0e0ff; border:1px solid #555; border-radius:4px;"
            "  padding:4px 10px; font-size:13px; min-width:30px; }"
            "QPushButton:hover { background:#3a3a6a; border-color:#88f; }"
            "QPushButton:pressed { background:#1a1a4a; }"
            "QPushButton#playBtn { background:#1e4a1e; color:#7f7; border-color:#484; font-size:15px; }"
            "QPushButton#playBtn:hover { background:#2a6a2a; }"
            "QSlider::groove:horizontal { background:#333; height:6px; border-radius:3px; }"
            "QSlider::handle:horizontal { background:#88f; width:14px; height:14px; margin:-4px 0;"
            "  border-radius:7px; border:1px solid #66c; }"
            "QSlider::sub-page:horizontal { background:#556; border-radius:3px; }"
            "QLabel { color:#aaa; font-family:Consolas; font-size:11px; min-width:90px; }"
            "QComboBox { background:#252545; color:#e0e0ff; border:1px solid #555; border-radius:4px;"
            "  padding:2px 6px; }"
        );
        QVBoxLayout* mediaVLayout = new QVBoxLayout(iffMediaBar);
        mediaVLayout->setContentsMargins(6, 4, 6, 4);
        mediaVLayout->setSpacing(4);

        // Top row: clip selector + time label
        QHBoxLayout* mediaTopRow = new QHBoxLayout();
        mediaTopRow->addWidget(new QLabel("Clip:"));
        iffClipCombo = new QComboBox();
        iffClipCombo->setMinimumWidth(120);
        mediaTopRow->addWidget(iffClipCombo);
        mediaTopRow->addStretch();
        iffTimeLabel = new QLabel("0.000 / 0.000 s");
        mediaTopRow->addWidget(iffTimeLabel);
        mediaVLayout->addLayout(mediaTopRow);

        // Scrubber
        iffScrubber = new QSlider(Qt::Horizontal);
        iffScrubber->setRange(0, 10000);
        iffScrubber->setValue(0);
        mediaVLayout->addWidget(iffScrubber);

        // Bottom row: transport buttons
        QHBoxLayout* mediaBtnRow = new QHBoxLayout();
        mediaBtnRow->setSpacing(6);
        iffBtnPrev = new QPushButton("\u23ee"); iffBtnPrev->setToolTip("Previous clip");
        iffBtnStepBack = new QPushButton("\u23ea"); iffBtnStepBack->setToolTip("Step backward one frame");
        iffBtnPlay = new QPushButton("\u25b6"); iffBtnPlay->setObjectName("playBtn"); iffBtnPlay->setToolTip("Play / Pause");
        iffBtnStepFwd = new QPushButton("\u23e9"); iffBtnStepFwd->setToolTip("Step forward one frame");
        iffBtnNext = new QPushButton("\u23ed"); iffBtnNext->setToolTip("Next clip");
        mediaBtnRow->addStretch();
        mediaBtnRow->addWidget(iffBtnPrev);
        mediaBtnRow->addWidget(iffBtnStepBack);
        mediaBtnRow->addWidget(iffBtnPlay);
        mediaBtnRow->addWidget(iffBtnStepFwd);
        mediaBtnRow->addWidget(iffBtnNext);
        mediaBtnRow->addStretch();
        mediaVLayout->addLayout(mediaBtnRow);

        iffMediaBar->hide();
        // Add the IFF media bar to the unified viewer toolbar row
        // (Row 2, below "Mode: ..."), so all view-mode buttons sit
        // at the same screen position.  The IFF bar is hidden by
        // default and shown only when an .iff file is loaded.
        iffMediaBar->setStyleSheet(
            "QWidget#iffMediaBar { background:transparent; }"
            "QPushButton { background:#3a3a3a; color:#ddd; border:1px solid #555; border-radius:3px;"
            "  padding:3px 10px; font-size:12px; min-width:30px; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton#playBtn { background:#1e4a1e; color:#7f7; border-color:#484; font-size:14px; }"
            "QPushButton#playBtn:hover { background:#2a6a2a; }"
            "QSlider::groove:horizontal { background:#333; height:6px; border-radius:3px; }"
            "QSlider::handle:horizontal { background:#88f; width:14px; height:14px; margin:-4px 0;"
            "  border-radius:7px; border:1px solid #66c; }"
            "QSlider::sub-page:horizontal { background:#556; border-radius:3px; }"
            "QLabel { color:#aaa; font-family:Consolas; font-size:11px; }"
            "QComboBox { background:#333; color:#ddd; border:1px solid #555; border-radius:3px;"
            "  padding:2px 6px; }"
        );

        // ── Audio bar (Mode 4) ──────────────────────────────────────────────────
        // The audio bar is a self-contained music player UI shown only when
        // the Mode combo is set to "Audio" and a .wav file is loaded.  It
        // owns its own transport state via Windows MCI; we just translate
        // button clicks into `mciSendString` calls and refresh the
        // scrubber / time label from a 100ms QTimer while playing.
        audioBar = new QWidget();
        audioBar->setObjectName("audioBar");
        audioBar->setStyleSheet(
            "QWidget#audioBar { background:#1a1a2e; border-top:1px solid #444; padding:4px; }"
            "QPushButton { background:#252545; color:#e0e0ff; border:1px solid #555; border-radius:4px;"
            "  padding:4px 10px; font-size:14px; min-width:32px; }"
            "QPushButton:hover { background:#3a3a6a; border-color:#88f; }"
            "QPushButton:pressed { background:#1a1a4a; }"
            "QPushButton#audioPlayBtn { background:#1e4a1e; color:#7f7; border-color:#484; font-size:16px; }"
            "QPushButton#audioPlayBtn:hover { background:#2a6a2a; }"
            "QPushButton:disabled { background:#2a2a2a; color:#555; border-color:#333; }"
            "QSlider::groove:horizontal { background:#333; height:6px; border-radius:3px; }"
            "QSlider::handle:horizontal { background:#88f; width:14px; height:14px; margin:-4px 0;"
            "  border-radius:7px; border:1px solid #66c; }"
            "QSlider::sub-page:horizontal { background:#556; border-radius:3px; }"
            "QLabel { color:#aaa; font-family:Consolas; font-size:11px; }"
        );
        QVBoxLayout* audioVLayout = new QVBoxLayout(audioBar);
        audioVLayout->setContentsMargins(6, 4, 6, 4);
        audioVLayout->setSpacing(4);

        // Top row: file label + time label
        QHBoxLayout* audioTopRow = new QHBoxLayout();
        audioFileLabel = new QLabel("(no audio loaded)");
        audioFileLabel->setStyleSheet("color:#cfc; font-family:Consolas; font-size:11px;");
        audioTopRow->addWidget(audioFileLabel);
        audioTopRow->addStretch();
        audioTimeLabel = new QLabel("0.000 / 0.000 s");
        audioTopRow->addWidget(audioTimeLabel);
        audioVLayout->addLayout(audioTopRow);

        // Scrubber
        audioScrubber = new QSlider(Qt::Horizontal);
        audioScrubber->setRange(0, 1000);
        audioScrubber->setValue(0);
        audioScrubber->setEnabled(false);
        audioVLayout->addWidget(audioScrubber);

        // Bottom row: transport buttons + volume
        QHBoxLayout* audioBtnRow = new QHBoxLayout();
        audioBtnRow->setSpacing(6);
        audioBtnBack   = new QPushButton("\u23ee"); audioBtnBack->setToolTip("Restart");
        audioBtnRewind = new QPushButton("\u23ea"); audioBtnRewind->setToolTip("Back 5 s");
        audioBtnPlay   = new QPushButton("\u25b6");
        audioBtnPlay->setObjectName("audioPlayBtn");
        audioBtnPlay->setToolTip("Play / Pause / Resume");
        audioBtnStop   = new QPushButton("\u25a0"); audioBtnStop->setToolTip("Stop");
        audioBtnFwd    = new QPushButton("\u23e9"); audioBtnFwd->setToolTip("Forward 5 s");
        audioBtnNext   = new QPushButton("\u23ed"); audioBtnNext->setToolTip("End");
        QLabel* volLbl = new QLabel("Vol:");
        audioVolume = new QSlider(Qt::Horizontal);
        audioVolume->setRange(0, 100);
        audioVolume->setValue(80);
        audioVolume->setFixedWidth(110);
        audioVolume->setToolTip("Volume");
        audioBtnRow->addStretch();
        audioBtnRow->addWidget(audioBtnBack);
        audioBtnRow->addWidget(audioBtnRewind);
        audioBtnRow->addWidget(audioBtnPlay);
        audioBtnRow->addWidget(audioBtnStop);
        audioBtnRow->addWidget(audioBtnFwd);
        audioBtnRow->addWidget(audioBtnNext);
        audioBtnRow->addSpacing(12);
        audioBtnRow->addWidget(volLbl);
        audioBtnRow->addWidget(audioVolume);
        audioBtnRow->addStretch();
        audioVLayout->addLayout(audioBtnRow);

        audioBar->hide();
        // Unified transparent styling (so the bar blends with the viewer
        // toolbar row that holds it).
        audioBar->setStyleSheet(
            "QWidget#audioBar { background:transparent; }"
            "QPushButton { background:#3a3a3a; color:#ddd; border:1px solid #555; border-radius:3px;"
            "  padding:3px 10px; font-size:12px; min-width:30px; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton#audioPlayBtn { background:#1e4a1e; color:#7f7; border-color:#484; font-size:14px; }"
            "QPushButton#audioPlayBtn:hover { background:#2a6a2a; }"
            "QPushButton:disabled { background:#2a2a2a; color:#555; border-color:#333; }"
            "QSlider::groove:horizontal { background:#333; height:6px; border-radius:3px; }"
            "QSlider::handle:horizontal { background:#88f; width:14px; height:14px; margin:-4px 0;"
            "  border-radius:7px; border:1px solid #66c; }"
            "QSlider::sub-page:horizontal { background:#556; border-radius:3px; }"
            "QLabel { color:#aaa; font-family:Consolas; font-size:11px; }"
        );

        // Connect the audio controls.
        connect(audioBtnBack,   &QPushButton::clicked, this, [this]() { audioSeek(0); });
        connect(audioBtnRewind, &QPushButton::clicked, this, [this]() { audioSkip(-kAudioSkipMs); });
        connect(audioBtnPlay,   &QPushButton::clicked, this, [this]() { audioPlayPauseResume(); });
        connect(audioBtnStop,   &QPushButton::clicked, this, [this]() { audioStop(); });
        connect(audioBtnFwd,    &QPushButton::clicked, this, [this]() { audioSkip(+kAudioSkipMs); });
        connect(audioBtnNext,   &QPushButton::clicked, this, [this]() { audioSeek(audioLengthMs()); });
        connect(audioScrubber,  &QSlider::sliderMoved, this, [this](int v) {
            int len = audioLengthMs();
            if (len > 0) audioSeek((int)((double)v / 1000.0 * len));
        });
        connect(audioVolume, &QSlider::valueChanged, this, [this](int v) {
            // MCI volume: 0..1000 (out of 1000).  Translate our 0..100.
            wchar_t cmd[128];
            std::swprintf(cmd, 128, L"setaudio igi1conv_wav volume to %d", v * 10);
            mciSendString(cmd, nullptr, 0, nullptr);
        });

        // Position-tick timer (drives the scrubber / time label while
        // playing).  100 ms is plenty for a smooth progress bar without
        // burning CPU.
        audioTimer = new QTimer(this);
        audioTimer->setInterval(100);
        connect(audioTimer, &QTimer::timeout, this, [this]() { audioRefreshFromMci(); });
        // ── Animation Panel (Mode 5) ──────────────────────────────────────────
        // Shows when the Mode combo is set to "Animation".  Combines the
        // 3D MEF viewer with the IFF animation timeline.  Layout:
        //   [Model: dropdown] [Animations: listbox] [▶ Play]  [loop] [fps 30]
        // The dropdown is populated with every ModelId found in the
        // decompiled objects.qsc; the listbox is populated with the
        // (boneHierarchy, standAnimation) pairs that belong to the
        // currently selected model.
        animationPanel = new QWidget();
        animationPanel->setObjectName("animationPanel");
        animationPanel->setStyleSheet(
            "QWidget#animationPanel { background:transparent; }"
            "QPushButton { background:#3a3a3a; color:#ddd; border:1px solid #555; border-radius:3px;"
            "  padding:1px 8px; font-size:11px; min-width:30px; max-height:20px; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QComboBox { background:#333; color:#ddd; border:1px solid #555; border-radius:3px; padding:1px 4px; max-height:20px; }"
            "QListWidget { background:#222; color:#ddd; border:1px solid #555; border-radius:3px;"
            "  font-family:Consolas; font-size:10px; padding:1px; max-height:60px; }"
            "QListWidget::item:selected { background:#0066aa; color:#fff; }"
            "QCheckBox { color:#ddd; font-family:Consolas; font-size:10px; spacing:3px; max-height:18px; }"
            "QCheckBox::indicator { width:12px; height:12px; }"
            "QLabel { color:#aaa; font-family:Consolas; font-size:10px; max-height:16px; }"
        );
        QHBoxLayout* animLayout = new QHBoxLayout(animationPanel);
        animLayout->setContentsMargins(0, 0, 0, 0);
        animLayout->setSpacing(4);

        animLayout->addWidget(new QLabel("Model:"));
        animModelCombo = new QComboBox();
        animModelCombo->setMinimumWidth(140);
        animModelCombo->setMaximumHeight(22);
        animModelCombo->setToolTip("Model IDs parsed from objects.qsc HumanSoldier Task_New calls");
        animLayout->addWidget(animModelCombo);

        animLayout->addSpacing(4);
        animLayout->addWidget(new QLabel("Anims:"));
        animAnimList = new QListWidget();
        animAnimList->setMinimumWidth(200);
        animAnimList->setMaximumHeight(60);
        animAnimList->setToolTip("Available (boneHierarchy, animation) pairs for the selected model");
        animLayout->addWidget(animAnimList);

        // No standalone Play button here: the shared IFF media bar above
        // already provides play/pause/forward/backward transport.  This
        // panel only drives the objects.qsc-based model/anim selector
        // and FPS input; double-clicking an entry in animAnimList loads
        // + plays it via onAnimationPlayClicked.

        animLoopChk = new QCheckBox("Loop");
        animLoopChk->setChecked(true);
        animLoopChk->setMaximumHeight(20);
        animLayout->addWidget(animLoopChk);

        animLayout->addWidget(new QLabel("FPS:"));
        animFpsInput = new QLineEdit("30");
        animFpsInput->setMaximumWidth(40);
        animFpsInput->setMaximumHeight(20);
        animFpsInput->setAlignment(Qt::AlignCenter);
        animFpsInput->setStyleSheet("QLineEdit { background:#333; color:#ddd; border:1px solid #555; border-radius:3px; padding:1px 2px; font-family:Consolas; font-size:10px; }");
        animFpsInput->setToolTip("Animation playback FPS (1-120)");
        animLayout->addWidget(animFpsInput);

        animStatusLabel = new QLabel("");
        animStatusLabel->setToolTip("Status of the last Animation operation");
        animLayout->addWidget(animStatusLabel);

        animLayout->addStretch();
        animationPanel->hide();
        viewerToolbarLayout->addWidget(animationPanel);

        // Wire the Animation panel signals.  The handlers live as member
        // methods on MainWindow so they can access the cached object set,
        // the temp ANIMS directory, and the modelViewer.
        connect(animModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &MainWindow::onAnimationModelChanged);
        // Double-click an entry in the anim list to load + play it.
        connect(animAnimList, &QListWidget::itemDoubleClicked,
                this, &MainWindow::onAnimationPlayClicked);
        connect(animFpsInput, &QLineEdit::editingFinished, this, [this]() {
            bool ok = false;
            int fps = animFpsInput->text().toInt(&ok);
            if (ok && modelViewer) {
                modelViewer->setAnimationFps(fps);
                animFpsInput->setText(QString::number(modelViewer->animationFps));
                if (g_logger)
                    g_logger(QString("[INFO] Animation FPS set to %1").arg(modelViewer->animationFps), LOG_INFO);
            }
        });
        viewerToolbarLayout->addWidget(iffMediaBar);
        // Audio bar lives in the same unified viewer toolbar
        // row as the IFF media bar so the music-player controls sit
        // at the same screen position regardless of which file type
        // is loaded.  It is hidden by default and only shown when a
        // .wav is opened in Audio mode.
        viewerToolbarLayout->addWidget(audioBar);

        // Wire media bar buttons
        connect(iffBtnPlay, &QPushButton::clicked, this, [this]() {
            modelViewer->iffTogglePlayPause();
            iffBtnPlay->setText(modelViewer->iffPlaying ? "\u23f8" : "\u25b6");
        });
        connect(iffBtnStepBack, &QPushButton::clicked, this, [this]() {
            modelViewer->iffPause();
            iffBtnPlay->setText("\u25b6");
            modelViewer->iffStepBackward();
        });
        connect(iffBtnStepFwd, &QPushButton::clicked, this, [this]() {
            modelViewer->iffPause();
            iffBtnPlay->setText("\u25b6");
            modelViewer->iffStepForward();
        });
        connect(iffBtnPrev, &QPushButton::clicked, this, [this]() {
            modelViewer->iffSetClip(modelViewer->iffGetClipIndex() - 1);
        });
        connect(iffBtnNext, &QPushButton::clicked, this, [this]() {
            modelViewer->iffSetClip(modelViewer->iffGetClipIndex() + 1);
        });
        connect(iffClipCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
            modelViewer->iffSetClip(idx);
        });
        connect(iffScrubber, &QSlider::sliderPressed, this, [this]() {
            iffScrubbing = true;
            modelViewer->iffPause();
            iffBtnPlay->setText("\u25b6");
        });
        connect(iffScrubber, &QSlider::sliderReleased, this, [this]() {
            iffScrubbing = false;
        });
        connect(iffScrubber, &QSlider::valueChanged, this, [this](int val) {
            if (!iffScrubbing) return;
            float t = (val / 10000.0f) * modelViewer->iffGetDuration();
            modelViewer->iffSeekTo(t);
        });

        // Wire ModelViewer callback -> update bar UI
        modelViewer->onIffTimeChanged = [this](float time, float duration, int clip, int clipCount) {
            if (iffScrubbing) return;
            if (iffScrubber) {
                iffScrubber->blockSignals(true);
                iffScrubber->setValue(duration > 0 ? (int)((time / duration) * 10000) : 0);
                iffScrubber->blockSignals(false);
            }
            if (iffTimeLabel) {
                // Duration is in milliseconds in the IFF format
                iffTimeLabel->setText(QString("%1 / %2 ms")
                    .arg((int)time).arg((int)duration));
            }
            if (iffClipCombo && iffClipCombo->count() != clipCount) {
                iffClipCombo->blockSignals(true);
                iffClipCombo->clear();
                for (int i = 0; i < clipCount; i++) {
                    int animId = modelViewer->iffGetClipAnimId(i);
                    iffClipCombo->addItem(animId >= 0
                        ? QString("Clip %1  (anim #%2)").arg(i + 1).arg(animId)
                        : QString("Clip %1").arg(i + 1));
                }
                iffClipCombo->blockSignals(false);
            }
            if (iffClipCombo && iffClipCombo->currentIndex() != clip) {
                iffClipCombo->blockSignals(true);
                iffClipCombo->setCurrentIndex(clip);
                iffClipCombo->blockSignals(false);
            }
        };

        // ── 3D Graph Toolbar (inline with Mode row) ──────────────────────────
        // The graph toolbar is now created ABOVE this point (in the
        // viewModeLayout row, next to "Mode: 3D").  We only need to
        // connect the button signals to the modelViewer state here,
        // because the lambdas capture `modelViewer` which is only
        // initialised below.

        // Debug Output
        QGroupBox* debugGroup = new QGroupBox("Conversion Debug Output");
        QVBoxLayout* debugLayout = new QVBoxLayout(debugGroup);
        consoleEdit = new QTextEdit();
        consoleEdit->setReadOnly(true);
        consoleEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        consoleEdit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(consoleEdit, &QTextEdit::customContextMenuRequested, this, [this](const QPoint& pos) {
            QMenu menu;
            menu.addAction("Copy", consoleEdit, &QTextEdit::copy);
            menu.addAction("Select All", consoleEdit, &QTextEdit::selectAll);
            menu.addSeparator();
            menu.addAction("Clear", consoleEdit, &QTextEdit::clear);
            menu.exec(consoleEdit->mapToGlobal(pos));
        });
        debugLayout->addWidget(consoleEdit);

        rightLayout->addWidget(debugGroup, 1);

        splitter->addWidget(leftWidget);
        splitter->addWidget(rightWidget);
        splitter->setSizes({300, 900});

        connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
            QModelIndex srcIndex = proxyModel->mapToSource(current);
            if (!fileModel->isDir(srcIndex)) {
                loadFile(fileModel->filePath(srcIndex));
            }
        });
        
        // Multi-select: copy/cut/paste using keyboard on selection
        QShortcut *copySelShortcut = new QShortcut(QKeySequence::Copy, treeView);
        connect(copySelShortcut, &QShortcut::activated, this, [this]() {
            auto indexes = treeView->selectionModel()->selectedRows();
            if (!indexes.isEmpty()) {
                clipboardFilePath = fileModel->filePath(proxyModel->mapToSource(indexes.first()));
                clipboardIsCut = false;
                logMessage(QString("[INFO] Copied %1 item(s) to clipboard").arg(indexes.size()));
            }
        });
        QShortcut *cutSelShortcut = new QShortcut(QKeySequence::Cut, treeView);
        connect(cutSelShortcut, &QShortcut::activated, this, [this]() {
            auto indexes = treeView->selectionModel()->selectedRows();
            if (!indexes.isEmpty()) {
                clipboardFilePath = fileModel->filePath(proxyModel->mapToSource(indexes.first()));
                clipboardIsCut = true;
                logMessage(QString("[INFO] Cut %1 item(s) to clipboard").arg(indexes.size()));
            }
        });
        QShortcut *deleteSelShortcut = new QShortcut(QKeySequence::Delete, treeView);
        connect(deleteSelShortcut, &QShortcut::activated, this, [this]() {
            auto indexes = treeView->selectionModel()->selectedRows();
            if (indexes.isEmpty()) return;
            if (QMessageBox::question(this, "Delete", QString("Delete %1 selected item(s)?").arg(indexes.size())) != QMessageBox::Yes) return;
            for (auto& idx : indexes) {
                QString path = fileModel->filePath(proxyModel->mapToSource(idx));
                if (QFileInfo(path).isDir()) QDir(path).removeRecursively();
                else QFile(path).remove();
            }
        });

        connect(viewModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            // Always reload currentFile on a mode change so the new
            // mode's hideAllViewers()/toolbar rules apply.
            if (!currentFile.isEmpty()) {
                loadFile(currentFile, index);
            }
        });

        // Lazily load the animation set on startup if a path was saved.
        // loadAnimationSetFromQsc() is robust to a missing path - it
        // auto-detects from LevelPath - so we just call it
        // unconditionally on startup when the mode is on.  This is
        // what populates the dropdown even when the user never opened
        // Settings > Animation explicitly.
        if (globalAnimationModeEnabled) {
            loadAnimationSetFromQsc();
        }

        hideAllViewers();
    }

    void closeEvent(QCloseEvent *event) override {
        // Stop any in-flight MCI playback so the app exits cleanly
        // (otherwise winmm can hold a handle open and Windows will
        // complain on shutdown).
        if (audioTimer) audioTimer->stop();
        mciClose();
        QDir tempDir(QDir::tempPath() + "/igi_temp_mef");
        if (tempDir.exists()) tempDir.removeRecursively();
        QMainWindow::closeEvent(event);
    }

    void logMessage(const QString& msg, LogLevel msgLevel = LOG_INFO) {
        QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
        QString levelStr = QSettings(iniPath, QSettings::IniFormat).value("LOGS_LEVEL", "INFO").toString();
        LogLevel currentLevel = LOG_INFO;
        if (levelStr == "ERROR") currentLevel = LOG_ERROR;
        else if (levelStr == "DEBUG") currentLevel = LOG_DEBUG;
        else if (levelStr == "VERBOSE") currentLevel = LOG_VERBOSE;
        
        if (msgLevel > currentLevel) return;
        
        QString prefix = "[INFO] ";
        if (msgLevel == LOG_ERROR) prefix = "[ERROR] ";
        else if (msgLevel == LOG_DEBUG) prefix = "[DEBUG] ";
        else if (msgLevel == LOG_VERBOSE) prefix = "[VERBOSE] ";
        
        QString finalMsg = msg;
        if (finalMsg.startsWith("[INFO] ")) finalMsg = finalMsg.mid(7);
        else if (finalMsg.startsWith("[ERROR] ")) finalMsg = finalMsg.mid(8);
        else if (finalMsg.startsWith("[DEBUG] ")) finalMsg = finalMsg.mid(8);
        else if (finalMsg.startsWith("[VERBOSE] ")) finalMsg = finalMsg.mid(10);
        
        finalMsg = prefix + finalMsg;
        
        if (consoleEdit) {
            consoleEdit->append(finalMsg);
        }
        if (QSettings(iniPath, QSettings::IniFormat).value("LOGS_ENABLED", true).toBool()) {
            QFile logFile(QCoreApplication::applicationDirPath() + "/igi1conv.log");
            if (logFile.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream out(&logFile);
                out << finalMsg << "\n";
            }
        }
    }

private:
    QFileSystemModel* fileModel;
    QSortFilterProxyModel* proxyModel;
    QTreeView* treeView;
    QLineEdit* fileSearchBox;
    QWidget* textSearchWidget;
    QLineEdit* textSearchBox;
    QComboBox* viewModeCombo;
    CodeEditor* viewerEdit;
    GraphHexEditor* graphHexEditor = nullptr;  // shown only for graph*.dat in Hex mode
    ImageEditor* imageEditor;
    ModelViewer* modelViewer;
    QWidget* iffMediaBar = nullptr;
    QPushButton* iffBtnPrev = nullptr;
    QPushButton* iffBtnStepBack = nullptr;
    QPushButton* iffBtnPlay = nullptr;
    QPushButton* iffBtnStepFwd = nullptr;
    QPushButton* iffBtnNext = nullptr;
    QSlider* iffScrubber = nullptr;
    QLabel* iffTimeLabel = nullptr;

    // ── Audio mode (Mode 4) ─────────────────────────────────────────────────
    // IGI ships audio as a proprietary ILSF container.  When the user
    // opens a .wav in Audio mode we transparently run `wav convert` to
    // produce a sibling <name>.playback.wav, then load it into MCI for
    // in-process playback.  The standard music-player controls live
    // here: play / pause / resume / stop / back / forward / scrub.
    QWidget* audioBar = nullptr;
    QPushButton* audioBtnBack = nullptr;    // ⏮  jump back
    QPushButton* audioBtnRewind = nullptr;  // ⏪  -5s
    QPushButton* audioBtnPlay = nullptr;    // ▶ / ⏸
    QPushButton* audioBtnStop = nullptr;    // ⏹
    QPushButton* audioBtnFwd = nullptr;     // ⏩  +5s
    QPushButton* audioBtnNext = nullptr;    // ⏭  jump forward (next file)
    QSlider* audioScrubber = nullptr;       // 0..1000 progress
    QSlider* audioVolume = nullptr;         // 0..100 volume
    QLabel* audioTimeLabel = nullptr;       // "1.234 / 5.000 s"
    QLabel* audioFileLabel = nullptr;       // current file
    QTimer* audioTimer = nullptr;           // updates scrubber while playing
    QString audioCurrentPath;               // wav currently loaded into MCI
    QString audioSourceName;                // original source name shown in UI
    bool    audioMciReady = false;          // true if the MCI alias is open
    static constexpr int kAudioSkipMs = 5000; // ±5s for back/forward

    // ── 3D Graph toolbar (shows when a graph.dat is loaded) ────────────────
    QWidget* graphToolbar = nullptr;
    QPushButton* btnGraphNodePlus = nullptr;
    QPushButton* btnGraphNodeMinus = nullptr;
    QCheckBox*   chkGraphNodes = nullptr;
    QCheckBox*   chkGraphLinks = nullptr;
    QLabel*      lblGraphTotalNodes = nullptr;
    QLabel*      lblGraphTotalLinks = nullptr;
    QPushButton* btnGraphReset = nullptr;
    QComboBox* iffClipCombo = nullptr;
    bool iffScrubbing = false;

    // ── Animation mode state (Mode 5) ──────────────────────────────────────
    // Gated by the Settings > Animation toggle (`globalAnimationModeEnabled`).
    // The QscObjectSet is the parsed view of the level's objects.qsc.
    // The temp directory under `globalCacheDir` holds the extracted
    // 000.IFF..006.IFF bones files so the playback hot path doesn't
    // have to hit the source game folder on every frame.
    QWidget*    animationPanel     = nullptr;
    QComboBox*  animModelCombo     = nullptr;
    QListWidget* animAnimList      = nullptr;
    QCheckBox*  animLoopChk        = nullptr;
    QLineEdit*  animFpsInput       = nullptr;
    QLabel*     animStatusLabel    = nullptr;

    QTimer*     anim30FpsTimer     = nullptr; // 30 FPS step driver
    bool        globalAnimationModeEnabled = false;
    QString     globalObjectsQscPath;        // path to decompiled objects.qsc
    QString     globalAnimsSourceDir;        // user-selected ANIMS source folder
    QString     globalAnimsCacheDir;         // <CacheDir>/animation_anims
    QString     globalModelsDir;             // LEVEL1\models
    igi1conv::QscObjectSet animationSet;     // parsed HumanSoldier entries

    QTextEdit* consoleEdit;
    QString currentFile;
    QString currentExt;
    QString clipboardFilePath;
    bool clipboardIsCut = false;
    QString globalLevelMtpPath;
    QString globalLevelDatPath;
    QString globalTextureDir;
    QString globalCacheDir;
    bool globalAutoSaveRes = false;

    void hideAllViewers() {
        viewerEdit->hide();
        imageEditor->hide();
        modelViewer->hide();
        if (iffMediaBar) iffMediaBar->hide();
        if (graphToolbar) graphToolbar->hide();
        if (graphHexEditor) graphHexEditor->hide();
        if (imageEditor && imageEditor->toolsWidget) imageEditor->toolsWidget->hide();
        if (animationPanel) animationPanel->hide();
        // Audio bar lives on the same unified viewer row as the IFF
        // media bar; hide it (and stop playback) whenever the user
        // switches mode.
        if (audioBar) audioBar->hide();
        if (audioTimer) audioTimer->stop();
        mciClose();
        // Stop animation and reset play button
        if (modelViewer) {
            modelViewer->iffPause();
            if (iffBtnPlay) iffBtnPlay->setText("\u25b6");
        }
        if (anim30FpsTimer) anim30FpsTimer->stop();
    }

    // ── Animation mode plumbing ─────────────────────────────────────────────
    //
    // Mode combo order is fixed so the View menu and context-menu "View As"
    // stay in sync: 0=Auto 1=Text 2=Hex 3=Image 4=Audio 5=Animation 6=3D.
    // "Animation" replaces the old "Video" mode and handles both .iff
    // skeletal animations and .mef model previews with playback controls.
    void rebuildModeCombo() {
        if (!viewModeCombo) return;
        int prev = viewModeCombo->currentIndex();
        viewModeCombo->blockSignals(true);
        viewModeCombo->clear();
        viewModeCombo->addItems({"Auto", "Text", "Hex", "Image", "Audio", "Animation", "3D"});
        viewModeCombo->setCurrentIndex(qMin(prev, viewModeCombo->count() - 1));
        viewModeCombo->blockSignals(false);
    }

    // Re-parse the decompiled objects.qsc and rebuild the model
    // dropdown.  Called when the user picks a new file, on app start
    // (if a path was saved), and from the "Reload Animation Set"
    // menu item.  When the path is missing the helper auto-detects
    // the level's objects.qsc / ANIMS / models folders and persists
    // them so the user does not have to do three manual picks.
    void loadAnimationSetFromQsc() {
        if (globalObjectsQscPath.isEmpty() || !QFile::exists(globalObjectsQscPath)) {
            // Try a few common locations before giving up.
            QStringList candidates;
            QString levelDir;
            QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
            levelDir = QSettings(iniPath, QSettings::IniFormat).value("LevelPath", "").toString();
            if (!levelDir.isEmpty()) {
                candidates << (levelDir + "/objects.qsc")
                           << (levelDir + "/OBJECTS.qsc");
                QDir p = QDir(levelDir); p.cdUp();
                if (p.exists("objects.qsc")) candidates << (p.absoluteFilePath("objects.qsc"));
                // IGI1 stores a single objects.qsc per *mission* (not
                // per level).  Scan the mission folder for any
                // objects.qsc under any LEVEL*/ subfolder - the user
                // may have set LevelPath=LEVEL6 but the file lives in
                // LEVEL1/objects.qsc.
                if (p.exists()) {
                    QDirIterator it(p.absolutePath(),
                                    QStringList() << "objects.qsc" << "OBJECTS.qsc",
                                    QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        QString p2 = it.next();
                        if (!candidates.contains(p2)) candidates << p2;
                    }
                }
            }
            for (const QString& c : candidates) {
                if (QFile::exists(c)) {
                    globalObjectsQscPath = c;
                    QSettings(iniPath, QSettings::IniFormat).setValue("ObjectsQscPath", c);
                    logMessage("[INFO] Animation auto-detected objects.qsc: " + c);
                    break;
                }
            }
            if (globalObjectsQscPath.isEmpty() || !QFile::exists(globalObjectsQscPath)) {
                logMessage("[WARN] Animation set: no objects.qsc set. Use Settings > Animation > Set Objects.qsc...");
                if (animStatusLabel) animStatusLabel->setText("No objects.qsc set - use Settings > Animation");
                return;
            }
        }
        // Lazily auto-detect the ANIMS source folder + models folder if
        // they're empty - this saves the user three extra dialog
        // round-trips on first use.  We never overwrite an explicit
        // setting.
        autoDetectAnimationFolders();
        QFile f(globalObjectsQscPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logMessage("[ERROR] Animation set: cannot read " + globalObjectsQscPath);
            return;
        }
        QString text = QString::fromUtf8(f.readAll());
        f.close();
        std::string err;
        animationSet = igi1conv::QscObjectSet::parse(text.toStdString(), &err);
        if (!err.empty()) {
            logMessage(QString("[WARN] Animation set parse: %1").arg(QString::fromStdString(err)));
        }
        logMessage(QString("[INFO] Animation set: parsed %1 HumanSoldier entries from %2")
            .arg(animationSet.entries.size())
            .arg(globalObjectsQscPath));
        populateAnimationModelCombo();
    }

    // Auto-detect the ANIMS source folder (e.g. <root>/IGI1/common/ANIMS)
    // and the LEVEL models folder (e.g. <root>/IGI1/missions/<loc>/<LEVEL>/models)
    // when the user hasn't picked them explicitly.  Uses the LevelPath
    // already saved in igi1conv.ini as the anchor: the ANIMS folder is
    // typically 4 levels up from a LEVEL folder (missions/location/LEVEL).
    void autoDetectAnimationFolders() {
        QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
        QSettings settings(iniPath, QSettings::IniFormat);
        QString levelDir = settings.value("LevelPath", "").toString();

        // ── Models folder ──────────────────────────────────────────────
        if (globalModelsDir.isEmpty() || !QDir(globalModelsDir).exists()) {
            QStringList cands;
            if (!levelDir.isEmpty()) {
                cands << (levelDir + "/models")
                      << (levelDir + "/MODELS")
                      << (levelDir + "/Models");
            }
            for (const QString& c : cands) {
                if (QDir(c).exists()) {
                    globalModelsDir = c;
                    settings.setValue("ModelsDir", c);
                    logMessage("[INFO] Animation auto-detected models folder: " + c);
                    break;
                }
            }
        }

        // ── ANIMS source folder ────────────────────────────────────────
        // IGI1 stores bone hierarchies at <root>/IGI1/common/ANIMS, where
        // <root> is the parent of the missions folder.  We walk up from
        // the LEVEL directory until we find an `ANIMS` sibling.
        if (globalAnimsSourceDir.isEmpty() || !QDir(globalAnimsSourceDir).exists()) {
            if (!levelDir.isEmpty()) {
                QDir d(levelDir);
                for (int i = 0; i < 6 && d.cdUp(); ++i) {
                    QStringList tryNames = {"common/ANIMS", "Common/ANIMS", "COMMON/ANIMS", "common/anims", "ANIMS"};
                    for (const QString& n : tryNames) {
                        QString c = d.absoluteFilePath(n);
                        if (QDir(c).exists()) {
                            globalAnimsSourceDir = c;
                            settings.setValue("AnimsSourceDir", c);
                            logMessage("[INFO] Animation auto-detected ANIMS folder: " + c);
                            goto doneAnims;
                        }
                    }
                }
                doneAnims:;
            }
        }

        // ── Cache folder: keep the default if user didn't override ────
        if (globalAnimsCacheDir.isEmpty()) {
            globalAnimsCacheDir = globalCacheDir + "/animation_anims";
            settings.setValue("AnimsCacheDir", globalAnimsCacheDir);
        }
    }

    void populateAnimationModelCombo() {
        if (!animModelCombo) return;
        animModelCombo->blockSignals(true);
        animModelCombo->clear();
        auto ids = animationSet.modelIds();
        for (const auto& id : ids) {
            // Annotate the dropdown with the count of available
            // animations so the user can see at a glance which models
            // actually have animations attached.
            auto anims = animationSet.animationsForModel(id);
            animModelCombo->addItem(QString::fromStdString(id) +
                QString("  (%1 anims)").arg(anims.size()));
        }
        animModelCombo->blockSignals(false);
        if (animModelCombo->count() > 0) {
            animModelCombo->setCurrentIndex(0);
            onAnimationModelChanged(0);
        } else {
            // No entries - clear the list and show a status hint.
            animAnimList->clear();
            if (animStatusLabel) {
                if (animationSet.entries.empty()) {
                    animStatusLabel->setText("No HumanSoldier entries - set objects.qsc");
                } else {
                    animStatusLabel->setText("No model IDs found in set");
                }
            }
        }
    }

    // Slot: called when the user picks a different model in the
    // dropdown.  Refreshes the animation listbox with the
    // (boneHierarchy, standAnimation) pairs that belong to this model
    // and enables the play button iff at least one animation is
    // available.
    void onAnimationModelChanged(int index) {
        if (!animAnimList) return;
        animAnimList->clear();
        if (index < 0 || index >= animModelCombo->count()) {
            return;
        }
        QString label = animModelCombo->itemText(index);
        // Strip the "(N anims)" suffix we appended in populateAnimationModelCombo.
        int sp = label.indexOf("  (");
        QString modelId = (sp > 0) ? label.left(sp) : label;
        auto anims = animationSet.animationsForModel(modelId.toStdString());
        for (const auto& a : anims) {
            animAnimList->addItem(QString("bh=%1  anim=%2")
                .arg(a.boneHierarchy, 3, 10, QChar('0'))
                .arg(a.standAnimation));
        }
        (void)anims; // anim list is already populated above
        if (anims.empty()) {
            animStatusLabel->setText("No animations for this model");
        } else {
            animStatusLabel->setText("");
        }
    }

    // Pre-extract every 000.IFF..006.IFF (or 3-digit) file from the
    // ANIMS source folder into the temp cache so playback doesn't
    // touch the source folder.  The extract is a plain QFile::copy
    // because the IFF files are not packed - they are already loose
    // on disk in IGI 1's game tree.
    void preExtractAllAnims() {
        if (globalAnimsSourceDir.isEmpty() || !QDir(globalAnimsSourceDir).exists()) {
            logMessage("[WARN] ANIMS source folder is not set. Use Settings > Animation.");
            return;
        }
        QDir srcDir(globalAnimsSourceDir);
        QStringList filters; filters << "*.IFF" << "*.iff" << "*.BFF" << "*.bff";
        QFileInfoList list = srcDir.entryInfoList(filters, QDir::Files);
        if (list.isEmpty()) {
            logMessage("[WARN] No IFF files found in " + globalAnimsSourceDir);
            return;
        }
        QDir().mkpath(globalAnimsCacheDir);
        int copied = 0, skipped = 0;
        for (const QFileInfo& fi : list) {
            QString dst = globalAnimsCacheDir + "/" + fi.fileName();
            if (QFile::exists(dst)) { ++skipped; continue; }
            if (QFile::copy(fi.absoluteFilePath(), dst)) ++copied;
        }
        logMessage(QString("[INFO] Pre-extract ANIMS: %1 copied, %2 already cached, %3 total (cache=%4)")
            .arg(copied).arg(skipped).arg(list.size()).arg(globalAnimsCacheDir));
    }

    // Slot: green Play button.  Looks up the MEF model on disk (with
    // 4 candidate extensions), then loads the bone-hierarchy IFF
    // (000.IFF, 001.IFF, ..., in the cache) and asks the model
    // viewer to play the requested clip.
    // Right-click "Play Animation" on a .MEF or .IFF file: switch to
    // Animation mode and load the file.  For .MEF we also try to select
    // the model in the animation panel so the user can pick a clip from
    // objects.qsc; for .IFF the clip selector in the media bar is used.
    void playAnimationForFile(const QString& path) {
        QString ext = QFileInfo(path).suffix().toLower();
        // Make this file the current file so the mode combo's index-changed
        // signal (if any) and the viewer both operate on the right path.
        currentFile = path;
        currentExt = ext;
        // Switch to Animation mode (index 5).  Block signals while changing
        // the combo so we don't accidentally reload the previous currentFile.
        if (viewModeCombo) {
            int animIdx = 5;
            if (viewModeCombo->count() <= animIdx) animIdx = viewModeCombo->count() - 1;
            viewModeCombo->blockSignals(true);
            viewModeCombo->setCurrentIndex(animIdx);
            viewModeCombo->blockSignals(false);
        }
        if (ext == "mef" || ext == "mex") {
            QString modelId = QFileInfo(path).completeBaseName();
            // Auto-load animation set if empty so the user can pick an animation
            if (animationSet.entries.empty()) {
                loadAnimationSetFromQsc();
            }
            // Try to find this modelId in the animation set and select it
            if (!animationSet.entries.empty()) {
                for (int i = 0; i < animModelCombo->count(); ++i) {
                    QString label = animModelCombo->itemText(i);
                    int sp = label.indexOf("  (");
                    QString id = (sp > 0) ? label.left(sp) : label;
                    if (id == modelId) {
                        animModelCombo->setCurrentIndex(i);
                        break;
                    }
                }
            }
            if (modelViewer) {
                modelViewer->loadModel(path);
                modelViewer->show();
            }
            logMessage("[INFO] Play Animation: loaded " + modelId + " - pick an animation and click Play");
            if (animStatusLabel) {
                if (animationSet.entries.empty()) {
                    animStatusLabel->setText("Set objects.qsc in Settings > Animation");
                } else {
                    animStatusLabel->setText("Pick an animation and click Play");
                }
            }
            if (animationPanel) animationPanel->show();
        } else if (ext == "iff" || ext == "bff") {
            if (modelViewer) {
                modelViewer->loadModel(path);
                modelViewer->show();
            }
            logMessage("[INFO] Play Animation: loaded IFF " + QFileInfo(path).fileName());
            if (animStatusLabel) animStatusLabel->setText("Playing IFF animation");
            if (animationPanel) animationPanel->show();
            if (iffMediaBar) iffMediaBar->show();
        }
    }

    // Right-click "Apply Animation on Model" on a .IFF/.BFF: show a list
    // of MEF model IDs found in the current level's models directory,
    // let the user pick one, then load the MEF and apply the IFF
    // animation to it.  The selected MEF's base name is used as the
    // model ID (e.g. "000_01_1").
    void applyIffOnModel(const QString& iffPath) {
        // Resolve the models directory.  Fall back to the folder
        // containing the IFF if no models dir is configured.
        QStringList searchDirs;
        if (!globalModelsDir.isEmpty() && QDir(globalModelsDir).exists())
            searchDirs << globalModelsDir;
        QFileInfo iffInfo(iffPath);
        if (iffInfo.absoluteDir().exists())
            searchDirs << iffInfo.absolutePath();

        if (searchDirs.isEmpty()) {
            QMessageBox::warning(this, "Apply Animation on Model",
                "No models directory configured.\n\n"
                "Set the LEVEL models folder via Settings > Animation > "
                "Set LEVEL Models Folder..., then try again.");
            return;
        }

        // Collect every *.mef / *.mex under the search dirs.
        QMap<QString, QString> modelIdToPath;
        QStringList modelIds;
        for (const QString& d : searchDirs) {
            QDirIterator it(d, QStringList() << "*.mef" << "*.MEF"
                                              << "*.mex" << "*.MEX",
                        QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString p = it.next();
                QString stem = QFileInfo(p).completeBaseName();
                if (!modelIdToPath.contains(stem)) {
                    modelIdToPath.insert(stem, p);
                    modelIds << stem;
                }
            }
        }
        modelIds.sort();

        // Filter to only show model IDs in the 000_00_0 – 030_00_0 range.
        QStringList filteredIds;
        for (const QString& id : modelIds) {
            if (id >= "000_00_0" && id <= "030_00_0") {
                filteredIds << id;
            }
        }
        modelIds = filteredIds;

        if (modelIds.isEmpty()) {
            QMessageBox::warning(this, "Apply Animation on Model",
                "No .mef / .mex models found in:\n" +
                searchDirs.join("\n"));
            return;
        }

        // Let the user pick a model.  Use QInputDialog::getItem for a
        // compact, searchable picker.
        bool ok = false;
        QString chosen = QInputDialog::getItem(
            this, "Apply Animation on Model",
            QString("Select a MEF model to apply\n%1\nto:")
                .arg(QFileInfo(iffPath).fileName()),
            modelIds, 0, true, &ok);
        if (!ok || chosen.isEmpty()) return;

        // Find the MEF path for the chosen model ID.
QString mefPath = modelIdToPath.value(chosen);
if (mefPath.isEmpty()) {
    QMessageBox::warning(this, "Apply Animation on Model",
        "Could not locate MEF file for model: " + chosen);
    return;
}

        // Make this the current file and switch to Animation mode so
        // the media bar / animation panel show.  Block signals to
        // avoid the combo's index-changed handler reloading a stale
        // currentFile (same guard as playAnimationForFile).
        currentFile = mefPath;
        currentExt = "mef";
        if (viewModeCombo) {
            int animIdx = 5;
            if (viewModeCombo->count() <= animIdx) animIdx = viewModeCombo->count() - 1;
            viewModeCombo->blockSignals(true);
            viewModeCombo->setCurrentIndex(animIdx);
            viewModeCombo->blockSignals(false);
        }

        // Load the MEF (provides the body mesh) then the IFF (provides
        // the bone animation).  loadIff() re-parses the IFF, computes
        // rest-pose bone transforms, and starts the play timer.
        if (modelViewer) {
            modelViewer->loadModel(mefPath);
            modelViewer->loadIff(iffPath);
            modelViewer->show();
        }
        if (animationPanel) animationPanel->show();
        if (iffMediaBar) iffMediaBar->show();

        logMessage("[INFO] Apply Animation on Model: IFF " +
            QFileInfo(iffPath).fileName() + " -> MEF " +
            QFileInfo(mefPath).fileName());
        if (animStatusLabel) {
            animStatusLabel->setText(QString("Playing %1 on %2")
                .arg(QFileInfo(iffPath).fileName(), chosen));
        }
    }

    // Right-click "Apply Lightmap" on a .mef: resolve its bound .olm
    // file(s) via objects.qsc (reusing the same path the Animation
    // settings already configure) and upload them as lightmap textures
    // in the viewer.  Silently no-ops (with a log line) if no binding or
    // files are found - this must never break loading a plain model.
    //
    // The same model id can be placed many times across a level (e.g. a
    // generic WaterTower mesh reused at several locations), each with
    // its own baked lightmap - so a model id is not always a unique key.
    // When more than one binding exists for this mef, the user picks
    // which placed instance's lightmap to apply.
    void applyLightmapOnModel(const QString& mefPath) {
        if (globalObjectsQscPath.isEmpty() || !QFile::exists(globalObjectsQscPath)) {
            logMessage("[WARN] Apply Lightmap: no objects.qsc set (Settings > Animation > Set Objects.qsc...)");
            return;
        }

        QFile qscFile(globalObjectsQscPath);
        if (!qscFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logMessage("[ERROR] Apply Lightmap: cannot read " + globalObjectsQscPath);
            return;
        }
        std::string qscText = QString::fromUtf8(qscFile.readAll()).toStdString();
        qscFile.close();

        std::string mefStem = QFileInfo(mefPath).completeBaseName().toStdString();
        igi1conv::LightmapBindingSet bindingSet = igi1conv::LightmapBindingSet::parse(qscText);
        auto matches = bindingSet.allBindingsForModel(mefStem);

        if (matches.empty()) {
            logMessage("[INFO] Apply Lightmap: no lightmap binding in " + QFileInfo(globalObjectsQscPath).fileName() +
                " for model " + QFileInfo(mefPath).fileName());
            return;
        }

        std::string chosenLogicalId = matches.front()->logicalId;
        if (matches.size() > 1) {
            QStringList options;
            for (auto* m : matches) {
                options << QString("%1 (task %2) -> %3")
                    .arg(m->taskName.empty() ? "(unnamed)" : QString::fromStdString(m->taskName))
                    .arg(m->taskId)
                    .arg(QString::fromStdString(m->logicalId));
            }
            bool ok = false;
            QString chosen = QInputDialog::getItem(
                this, "Apply Lightmap",
                QString("Model %1 is placed at %2 locations - pick which one's lightmap to apply:")
                    .arg(QFileInfo(mefPath).fileName()).arg(matches.size()),
                options, 0, false, &ok);
            if (!ok) return;
            int idx = options.indexOf(chosen);
            chosenLogicalId = matches[idx >= 0 ? idx : 0]->logicalId;
        }

        std::vector<std::string> olmFiles =
            igi1conv::ResolveLightmapFilesForLogicalId(globalObjectsQscPath.toStdString(), chosenLogicalId);

        if (olmFiles.empty()) {
            logMessage("[WARN] Apply Lightmap: binding " + QString::fromStdString(chosenLogicalId) +
                " found but no .olm files on disk for " + QFileInfo(mefPath).fileName());
            return;
        }

        if (modelViewer) {
            modelViewer->loadModel(mefPath);
            modelViewer->applyLightmapTextures(mefPath, olmFiles);
            modelViewer->show();
        }
        logMessage("[INFO] Apply Lightmap: " + QFileInfo(mefPath).fileName() +
            " <- " + QString::number((int)olmFiles.size()) + " lightmap file(s)");
    }

    void onAnimationPlayClicked() {
        if (!animModelCombo || !animAnimList) return;
        // Lazy: if the set is empty (e.g. the user just enabled the
        // mode and never set a path), try auto-detect one more time
        // before giving up.
        if (animationSet.entries.empty()) {
            loadAnimationSetFromQsc();
            if (animationSet.entries.empty()) {
                animStatusLabel->setText("Set objects.qsc in Settings > Animation");
                QMessageBox::information(this, "Animation Mode",
                    "No HumanSoldier entries loaded.\n\n"
                    "Open Settings > Animation and pick:\n"
                    "  - objects.qsc (decompiled from objects.qvm)\n"
                    "  - common/ANIMS folder (bone hierarchies)\n"
                    "  - LEVEL/models folder (MEF bodies)\n\n"
                    "Or set a level first via Settings > Level, then "
                    "toggle Animation mode - the paths are auto-detected.");
                return;
            }
        }
        int mi = animModelCombo->currentIndex();
        int li = animAnimList->currentRow();
        if (mi < 0 || li < 0) {
            animStatusLabel->setText("Pick a model + animation first");
            return;
        }
        QString label = animModelCombo->itemText(mi);
        int sp = label.indexOf("  (");
        QString modelId = (sp > 0) ? label.left(sp) : label;
        auto anims = animationSet.animationsForModel(modelId.toStdString());
        if (li >= (int)anims.size()) return;
        int bh = anims[li].boneHierarchy;
        int sa = anims[li].standAnimation;
        animStatusLabel->setText(QString("Loading %1 / bh=%2 / anim=%3 ...")
            .arg(modelId).arg(bh).arg(sa));

        // Resolve MEF path (search 4 extensions + LEVEL1/models)
        QStringList modelDirs;
        if (!globalModelsDir.isEmpty()) modelDirs << globalModelsDir;
        QStringList mefExts = {".mef", ".MEF", ".mex", ".MEX"};
        QString mefPath;
        for (const QString& d : modelDirs) {
            for (const QString& e : mefExts) {
                QString p = d + "/" + modelId + e;
                if (QFile::exists(p)) { mefPath = p; break; }
            }
            if (!mefPath.isEmpty()) break;
        }
        if (mefPath.isEmpty()) {
            animStatusLabel->setText("MEF not found: " + modelId);
            logMessage("[WARN] Animation: no MEF/ME model for " + modelId);
            return;
        }

        // Resolve IFF path.  3-digit padded name (000.IFF, 001.IFF, ...).
        QString bhName = QString("%1").arg(bh, 3, 10, QChar('0'));
        QStringList iffDirs;
        if (!globalAnimsCacheDir.isEmpty() && QDir(globalAnimsCacheDir).exists()) iffDirs << globalAnimsCacheDir;
        if (!globalAnimsSourceDir.isEmpty() && QDir(globalAnimsSourceDir).exists()) iffDirs << globalAnimsSourceDir;
        QStringList iffExts = {".IFF", ".iff", ".BFF", ".bff"};
        QString iffPath;
        for (const QString& d : iffDirs) {
            for (const QString& e : iffExts) {
                QString p = d + "/" + bhName + e;
                if (QFile::exists(p)) { iffPath = p; break; }
            }
            if (!iffPath.isEmpty()) break;
        }
        if (iffPath.isEmpty()) {
            animStatusLabel->setText("IFF not found: " + bhName);
            logMessage("[WARN] Animation: no IFF bone file for bh=" + bhName);
            return;
        }

        // Auto-apply textures for the MEF, so the user sees the
        // proper textures (like in 3D mode's "Apply Textures").
        // This is a non-blocking call: we copy any pre-existing
        // bundled textures from the cache to the model's folder
        // synchronously, but if the bundle is missing we kick off
        // a background `mef bundle` and let it finish asynchronously
        // (the model renders without textures while it runs).
        QString mefBase = QFileInfo(mefPath).completeBaseName();
        QString bundleDir = globalCacheDir + "/bundle/" + mefBase;
        if (!QDir(bundleDir).exists()
            && !globalLevelDatPath.isEmpty()
            && !globalTextureDir.isEmpty()) {
            logMessage("[INFO] Animation: pre-extracting textures for " + mefBase + "...");
            QProcess::execute(qApp->applicationFilePath(),
                QStringList() << "mef" << "bundle" << mefPath
                              << "-o" << (globalCacheDir + "/bundle")
                              << "--dat" << globalLevelDatPath
                              << "--texdir" << globalTextureDir
                              << "--no-obj");
        }

        // Load the MEF first (so the body is in the 3D viewer), then
        // load the IFF on top so the bone-driven skeleton animation
        // can drive the model.  loadIff() calls IFF_Parse and flips
        // the viewer into IFF-animation mode.
        if (modelViewer) {
            modelViewer->loadModel(mefPath);
            // After loadModel, the viewer holds the MEF mesh but
            // isIffAnimation is false.  loadIff() then swaps in the
            // skeleton, drives the body bones, and starts the
            // 30 FPS timer.
            modelViewer->loadIff(iffPath);

            // Debug: log bone count comparison and verify skeleton mapping
            logMessage(QString("[DBG] MEF bones: %1, IFF bones: %2, restMesh verts: %3")
                .arg(modelViewer->mefBoneWorldPos.size())
                .arg(modelViewer->currentIff.skeleton.bone_count)
                .arg(modelViewer->restMesh.size()));
            if (!modelViewer->mefBoneWorldPos.empty() && !modelViewer->iffRestBoneMats.empty()) {
                int n = std::min((int)modelViewer->mefBoneWorldPos.size(),
                                 (int)modelViewer->iffRestBoneMats.size());
                QVector3D iffRoot = modelViewer->iffRestBoneMats[0].map(QVector3D(0,0,0));
                glm::vec3 mefRoot = modelViewer->mefBoneWorldPos[0] / 40.96f;
                QVector3D rootOffset(mefRoot.x - iffRoot.x(), mefRoot.y - iffRoot.y(), mefRoot.z - iffRoot.z());
                int mismatchCount = 0;
                float maxDiff = 0.0f;
                int maxDiffBone = -1;
                for (int i = 0; i < n; ++i) {
                    QVector3D iffPos = modelViewer->iffRestBoneMats[i].map(QVector3D(0,0,0));
                    glm::vec3 mefPosG = modelViewer->mefBoneWorldPos[i] / 40.96f;
                    QVector3D mefPos(mefPosG.x, mefPosG.y, mefPosG.z);
                    float diff = (mefPos - (iffPos + rootOffset)).length();
                    if (diff > maxDiff) { maxDiff = diff; maxDiffBone = i; }
                    if (diff > 0.5f) mismatchCount++;
                    if (i < 5) {
                        logMessage(QString("[DBG] bone %1: MEF=(%2,%3,%4) IFF=(%5,%6,%7) d=%8")
                            .arg(i).arg(mefPos.x(),0,'f',3).arg(mefPos.y(),0,'f',3).arg(mefPos.z(),0,'f',3)
                            .arg(iffPos.x(),0,'f',3).arg(iffPos.y(),0,'f',3).arg(iffPos.z(),0,'f',3)
                            .arg(diff,0,'f',3));
                    }
                }
                if (mismatchCount > 0) {
                    logMessage(QString("[WARN] Bone mapping mismatch: %1 bones differ by >0.5 (worst bone %2 diff %3)")
                        .arg(mismatchCount).arg(maxDiffBone).arg(maxDiff,0,'f',3));
                } else {
                    logMessage(QString("[DBG] Bone mapping OK, max diff %1 (bone %2)").arg(maxDiff,0,'f',3).arg(maxDiffBone));
                }
            }
            // Find the clip index whose animation_id == sa and play it.
            int clipCount = modelViewer->iffGetClipCount();
            for (int i = 0; i < clipCount; ++i) {
                if (modelViewer->iffGetClipAnimId(i) == sa) {
modelViewer->playClip(i);
if (iffBtnPlay) iffBtnPlay->setText("\u23f8");
if (iffMediaBar) iffMediaBar->show();
animStatusLabel->setText(QString("Playing %1 / bh=%2 / anim=%3 (clip %4/%5) %6 FPS")
    .arg(modelId).arg(bh).arg(sa).arg(i+1).arg(clipCount).arg(modelViewer->animationFps));
                    logMessage(QString("[INFO] Animation playing: model=%1 bh=%2 anim=%3 clip=%4")
                        .arg(modelId).arg(bh).arg(sa).arg(i));
                    return;
                }
            }
            // If the requested clip id isn't present, fall back to
            // clip 0 so the user sees something animate (better UX
            // than an empty screen).
            if (clipCount > 0) {
modelViewer->playClip(0);
if (iffBtnPlay) iffBtnPlay->setText("\u23f8");
if (iffMediaBar) iffMediaBar->show();
animStatusLabel->setText(QString("Playing fallback clip 0 (anim %1 not found)")
    .arg(sa));
                logMessage(QString("[WARN] Animation: anim %1 not in %2, falling back to clip 0")
                    .arg(sa).arg(bhName));
            } else {
                animStatusLabel->setText("No clips in IFF: " + bhName);
            }
        }
    }

    // ── wav helpers ────────────────────────────────────────────────────────
    //
    // All three actions below route through the `wav` subcommand so they
    // behave identically for the IGI ILSF container and for any standard
    // .wav the user has dropped into the project.  Playback is implemented
    // by writing a sibling <file>.playback.wav and handing the URL to
    // QDesktopServices - the OS picks the default media player
    // (Windows Media Player, VLC, foobar2000, ...).  Re-playing the same
    // file overwrites the previous copy; we never accumulate junk.

    // Run `igi1conv wav convert <src> -o <dst>` and surface the result
    // via the log + a modal message box on error.  `outPath` may be a
    // file or a directory (the CLI handles both).
    // Returns true on success.
    bool runWavConvert(const QString& src, const QString& outPath)
    {
        QProcess proc;
        proc.setProgram(qApp->applicationFilePath());
        proc.setArguments(QStringList() << "wav" << "convert" << src << "-o" << outPath);
        proc.start();
        if (!proc.waitForFinished(-1)) {
            logMessage("[ERROR] wav convert: process did not finish in time");
            QMessageBox::critical(this, "Conversion failed",
                                  "igi1conv wav convert did not finish in time.");
            return false;
        }
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            return true;
        }
        QString err = proc.readAllStandardError().trimmed();
        if (err.isEmpty()) err = proc.readAllStandardOutput().trimmed();
        if (err.isEmpty()) err = QString("exit code %1").arg(proc.exitCode());
        logMessage("[ERROR] wav convert failed: " + err);
        QMessageBox::critical(this, "Conversion failed", err);
        return false;
    }

    // Compute a stable, content-addressed cache filename for a source
    // .wav.  Key is SHA256(file contents) + last-modified msec + the
    // output extension.  Re-encoding the same file is instant because
    // the cache key only changes when the contents change.
    QString wavCacheKey(const QString& srcPath, const QString& outExt) const
    {
        QFile f(srcPath);
        if (!f.open(QIODevice::ReadOnly)) return {};
        QByteArray hash = QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
        f.close();
        QFileInfo fi(srcPath);
        qint64 msec = fi.lastModified().toMSecsSinceEpoch();
        return QString::fromLatin1(hash.toHex().left(16)) + "_" +
               QString::number(msec, 16) + outExt;
    }

    // Single cache directory for everything (audio, textures, models,
    // animations).  Falls back to <temp>/igi_temp_mef if the user
    // hasn't set one yet (the constructor pre-creates the directory).
    // The user picks this once via Settings > Cache Folder... and
    // every subsystem (audio playback, MEF bundling, animation cache,
    // etc.) lands its files there.
    QString wavCacheDir() const
    {
        QString d = globalCacheDir;
        if (d.isEmpty()) d = QDir::tempPath() + "/igi_temp_mef";
        QDir().mkpath(d);
        return d;
    }

    // Convert `srcPath` (IGI .wav or standard WAV) to the chosen
    // extension, landing in the user-configured audio cache directory.
    // Returns the cached output path, or an empty string on error.
    // Re-opening the same source file is instant - the output path
    // is stable across runs as long as the source file is unchanged.
    QString cachedIgiWavConvert(const QString& srcPath, const QString& outExt)
    {
        if (!QFile::exists(srcPath)) {
            logMessage("[ERROR] cachedIgiWavConvert: not found: " + srcPath);
            return {};
        }
        const QString key = wavCacheKey(srcPath, outExt);
        if (key.isEmpty()) {
            logMessage("[ERROR] cachedIgiWavConvert: failed to hash: " + srcPath);
            return {};
        }
        const QString outPath = wavCacheDir() + "/" + key;
        if (QFile::exists(outPath)) {
            logMessage("[INFO] Audio cache hit: " + outPath);
            return outPath;
        }
        logMessage("[INFO] Audio cache miss, converting: " + srcPath + " -> " + outPath);
        if (!runWavConvert(srcPath, outPath)) return {};
        return outPath;
    }

    // Convert the IGI .wav at `path` to a cached <key>.wav in the
    // audio cache directory and open it with the system's default
    // media player.  Identical inputs hit the cache instantly.
    void playIgiWav(const QString& path)
    {
        QString outPath = cachedIgiWavConvert(path, ".wav");
        if (outPath.isEmpty()) return;
        logMessage("[SUCCESS] Converted for playback: " + outPath);
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(outPath))) {
            QMessageBox::warning(this, "Playback",
                "Converted to:\n" + outPath +
                "\n\nbut the system could not launch a media player for it.\n"
                "Open it manually or install a media player (e.g. VLC).");
        }
    }

    // Right-click "Convert to .wav (Windows PCM)..." -> standard
    // Save As dialog.  The user picks the exact destination file
    // (and can create a new folder from inside the dialog).  The
    // file is decoded straight from the ILSF / ADPCM source and
    // written as standard PCM .wav.  No caching - the file lands
    // wherever the user pointed.
    //
    // (`Play in default media player` still uses the SHA-256 cache
    // so re-plays are instant.)
    void convertIgiWav(const QString& path)
    {
        const QFileInfo fi(path);
        const QString defDir = fi.absolutePath();
        const QString defFile = fi.completeBaseName() + ".wav";
        QString outPath = QFileDialog::getSaveFileName(this,
            "Save Decoded Audio As",
            defDir + "/" + defFile,
            "Windows PCM WAV (*.wav)");
        if (outPath.isEmpty()) return;  // user cancelled

        // Defensive: the dialog filters on .wav, but the user can
        // type a different extension.  Force .wav so the file is
        // actually playable.
        if (!outPath.endsWith(".wav", Qt::CaseInsensitive))
            outPath += ".wav";

        std::string err;
        // encode_pcm_to_mp3 was removed when we dropped LAME; only
        // .wav is supported.  We hit runWavConvert directly.
        if (!runWavConvert(path, outPath)) {
            // runWavConvert already showed a modal error dialog.
            return;
        }
        logMessage("[SUCCESS] wav convert: " + path + " -> " + outPath);
        QMessageBox::information(this, "Converted", "Saved as:\n" + outPath);
    }

    // Batch-convert every *.wav under `dir` (recursive) to <outDir>,
    // preserving the directory tree.  Output is always standard PCM
    // .wav in this build (no MP3 / no external DLL).  Mirrors
    // `igi1conv wav convert-dir`.
    void convertIgiWavDir(const QString& dir)
    {
        const QString outExt = ".wav";
        QString outDir = QFileDialog::getExistingDirectory(
            this, "Choose output folder for converted .wav files",
            QFileInfo(dir).absolutePath());
        if (outDir.isEmpty()) return;

        QProcess proc;
        proc.setProgram(qApp->applicationFilePath());
        proc.setArguments(QStringList() << "wav" << "convert-dir" << dir << "-o" << outDir);
        proc.start();
        proc.waitForFinished(-1);
        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            logMessage("[SUCCESS] wav convert-dir: " + dir + " -> " + outDir + outExt);
            QMessageBox::information(this, "Batch Converted",
                "All decodable .wav files under\n" + dir +
                "\nwere converted to " + outExt + " in\n" + outDir);
        } else {
            QString err = proc.readAllStandardError().trimmed();
            if (err.isEmpty()) err = proc.readAllStandardOutput().trimmed();
            logMessage("[ERROR] wav convert-dir failed: " + err);
            QMessageBox::critical(this, "Batch Conversion failed", err);
        }
    }

    // Recursively count *.wav files under `dir`.  Used to decide whether
    // to expose the "Batch Convert" submenu in the right-click menu of
    // a folder.  We walk the tree ourselves (rather than spawning a
    // igi1conv process just to ask) so the menu decision is instant and
    // doesn't litter the log.
    int countWavFilesRec(const QString& dir, int depth = 0) const
    {
        if (depth > 8) return 0;  // safety: don't walk absurdly deep trees
        QDir d(dir);
        int n = d.entryList(QStringList() << "*.wav",
                            QDir::Files | QDir::Hidden).size();
        for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            n += countWavFilesRec(sub.absoluteFilePath(), depth + 1);
        }
        return n;
    }

    // ── Audio mode (MCI-backed music player) ────────────────────────────────
    //
    // All transport is driven by Windows MCI.  The "igi1conv_wav" alias
    // is created once when a file is opened and reused across play /
    // pause / resume / seek / stop.  Helpers below return -1 when MCI
    // reports an error and the caller surfaces the error string in the
    // log + a status label.

    // The MCI alias we use for every playback.  Fixed so play / pause
    // / resume / seek all hit the same device without races.
    static constexpr const wchar_t* kMciAlias = L"igi1conv_wav";

    // Run an MCI command that doesn't return a string.  Returns true on
    // success; on failure the error is logged.
    bool mciRun(const wchar_t* cmd)
    {
        MCIERROR err = mciSendString(cmd, nullptr, 0, nullptr);
        if (err != 0) {
            wchar_t errBuf[256] = {};
            mciGetErrorString(err, errBuf, 256);
            logMessage(QString("[ERROR] MCI '%1' failed: %2")
                .arg(QString::fromWCharArray(cmd), QString::fromWCharArray(errBuf)));
            return false;
        }
        return true;
    }

    // Send an MCI command that returns a string (e.g. "status ... length").
    // Returns the trimmed UTF-16 string, or empty on error.
    QString mciQuery(const wchar_t* cmd)
    {
        wchar_t buf[256] = {};
        MCIERROR err = mciSendString(cmd, buf, 256, nullptr);
        if (err != 0) return {};
        return QString::fromWCharArray(buf).trimmed();
    }

    // Close the MCI alias if it's open, ignoring errors so we can call
    // this repeatedly.
    void mciClose()
    {
        if (!audioMciReady) return;
        mciRun(L"stop igi1conv_wav");
        mciRun(L"close igi1conv_wav");
        audioMciReady = false;
    }

    // Open `wavPath` into MCI.  Converts to a Windows-friendly absolute
    // path and quotes it.  Returns true on success.
    bool mciOpen(const QString& wavPath)
    {
        mciClose();
        QString absPath = QFileInfo(wavPath).absoluteFilePath();
        // mciSendString wants a command line in UTF-16 (Windows wide
        // strings on the host platform).  QFileInfo gives us forward
        // slashes; MCI accepts either.
        std::wstring cmd = L"open \"";
        cmd += absPath.toStdWString();
        cmd += L"\" type waveaudio alias igi1conv_wav";
        if (!mciRun(cmd.c_str())) return false;
        audioMciReady = true;
        audioCurrentPath = wavPath;
        if (audioFileLabel) {
            // Show the original source file name, not the cached temp file name.
            audioFileLabel->setText(audioSourceName.isEmpty() ? QFileInfo(wavPath).fileName() : audioSourceName);
        }
        if (audioScrubber) audioScrubber->setEnabled(true);
        if (audioTimeLabel) {
            int lenMs = audioLengthMs();
            audioTimeLabel->setText(QString("0.000 / %1 s")
                .arg(lenMs / 1000.0, 0, 'f', 3));
        }
        return true;
    }

    // Total length of the currently-loaded WAV in milliseconds, or 0.
    int audioLengthMs() const
    {
        if (!audioMciReady) return 0;
        // mciQuery is non-const; cast through to keep this read-only
        // helper const-friendly for callers that don't mutate state.
        QString s = const_cast<MainWindow*>(this)->mciQuery(L"status igi1conv_wav length");
        return s.toInt();
    }

    // Current position in ms, or 0.
    int audioPositionMs() const
    {
        if (!audioMciReady) return 0;
        QString s = const_cast<MainWindow*>(this)->mciQuery(L"status igi1conv_wav position");
        return s.toInt();
    }

    // MCI mode string -> one of: "playing", "paused", "stopped".
    QString audioMode() const
    {
        if (!audioMciReady) return QString();
        return const_cast<MainWindow*>(this)->mciQuery(L"status igi1conv_wav mode");
    }

    // Drive the scrubber + time label from MCI's current state.  Called
    // by the position-tick timer while playing, and after every
    // transport action so the UI stays in sync.
    void audioRefreshFromMci()
    {
        if (!audioMciReady) return;
        int lenMs = audioLengthMs();
        int posMs = audioPositionMs();
        if (audioScrubber) {
            int v = (lenMs > 0) ? (int)((double)posMs / lenMs * 1000.0) : 0;
            v = std::clamp(v, 0, 1000);
            // Only update if the user isn't currently dragging it.
            if (audioScrubber->isSliderDown() == false)
                audioScrubber->setValue(v);
        }
        if (audioTimeLabel) {
            audioTimeLabel->setText(QString("%1 / %2 s")
                .arg(posMs / 1000.0, 0, 'f', 3)
                .arg(lenMs / 1000.0, 0, 'f', 3));
        }
        // Auto-stop when the file ends.
        QString mode = audioMode();
        if (mode == "stopped" && audioTimer && audioTimer->isActive() &&
            posMs > 0 && lenMs > 0 && posMs >= lenMs) {
            audioTimer->stop();
            if (audioBtnPlay) audioBtnPlay->setText("\u25b6");
        }
    }

    // Toggle play / pause / resume so the single "play" button behaves
    // like a standard music player.
    void audioPlayPauseResume()
    {
        if (!audioMciReady) return;
        QString mode = audioMode();
        if (mode == "playing") {
            mciRun(L"pause igi1conv_wav");
            if (audioBtnPlay) audioBtnPlay->setText("\u25b6");
            if (audioTimer) audioTimer->stop();
        } else if (mode == "paused") {
            mciRun(L"resume igi1conv_wav");
            if (audioBtnPlay) audioBtnPlay->setText("\u23f8");
            if (audioTimer) audioTimer->start();
        } else {
            mciRun(L"play igi1conv_wav");
            if (audioBtnPlay) audioBtnPlay->setText("\u23f8");
            if (audioTimer) audioTimer->start();
        }
        audioRefreshFromMci();
    }

    void audioStop()
    {
        if (!audioMciReady) return;
        mciRun(L"stop igi1conv_wav");
        mciRun(L"seek igi1conv_wav to start");
        if (audioBtnPlay) audioBtnPlay->setText("\u25b6");
        if (audioTimer) audioTimer->stop();
        audioRefreshFromMci();
    }

    // Seek by an absolute number of milliseconds, clamped to [0, length].
    void audioSeek(int posMs)
    {
        if (!audioMciReady) return;
        int lenMs = audioLengthMs();
        if (lenMs <= 0) return;
        posMs = std::clamp(posMs, 0, lenMs);
        wchar_t cmd[64];
        std::swprintf(cmd, 64, L"seek igi1conv_wav to %d", posMs);
        mciRun(cmd);
        audioRefreshFromMci();
    }

    // Skip by a relative offset (in ms); negative = backwards.
    void audioSkip(int deltaMs)
    {
        if (!audioMciReady) return;
        audioSeek(audioPositionMs() + deltaMs);
    }

    // Public entry point: load `srcWav` (an IGI .wav file path) into
    // the audio bar.  Runs the IGI->Windows PCM conversion first so the
    // MCI alias can open the result.  `autoPlay` starts playback as
    // soon as the file is ready.
    void audioLoadIgiWav(const QString& srcWav, bool autoPlay = false)
    {
        QFileInfo fi(srcWav);
        if (!fi.exists()) {
            logMessage("[ERROR] audioLoadIgiWav: not found: " + srcWav);
            return;
        }
        // Guard against non-audio files (e.g. a .mef selected while Audio
        // mode is active).  Only .wav files can be decoded/played here.
        if (fi.suffix().toLower() != "wav") {
            logMessage("[WARN] Audio mode: " + fi.fileName() + " is not a .wav file");
            if (audioFileLabel) audioFileLabel->setText(fi.fileName() + " (not audio)");
            return;
        }
        audioSourceName = fi.fileName();
        // Route through the audio cache: SHA256(src) + mtime keyed,
        // so re-opening the same file is instant.  The cached file
        // is the standard PCM .wav the MCI alias can play directly.
        QString outPath = cachedIgiWavConvert(srcWav, ".wav");
        if (outPath.isEmpty()) {
            return;
        }
        if (!mciOpen(outPath)) {
            return;
        }
        logMessage("[SUCCESS] Audio mode loaded: " + outPath);
        if (autoPlay) audioPlayPauseResume();
    }

    void showContextMenu(const QPoint& pos) {
        QModelIndex index = treeView->indexAt(pos);
        QModelIndex srcIndex = proxyModel->mapToSource(index);
        if (!srcIndex.isValid()) return;

        QString path = fileModel->filePath(srcIndex);
        QString ext = QFileInfo(path).suffix().toLower();
        bool isDir = fileModel->isDir(srcIndex);

        // Get all selected items
        auto selectedIndexes = treeView->selectionModel()->selectedRows();
        bool multiSelect = selectedIndexes.size() > 1;

        QMenu menu;
        
        // Multi-selection operations
        if (multiSelect) {
            menu.addAction(QString("Selected %1 items").arg(selectedIndexes.size()))->setEnabled(false);
            menu.addSeparator();
            menu.addAction("Delete Selected", [this, selectedIndexes]() {
                if (QMessageBox::question(this, "Delete", QString("Delete %1 selected items?").arg(selectedIndexes.size())) != QMessageBox::Yes) return;
                for (auto& idx : selectedIndexes) {
                    QString p = fileModel->filePath(proxyModel->mapToSource(idx));
                    if (QFileInfo(p).isDir()) QDir(p).removeRecursively();
                    else QFile(p).remove();
                }
            });
            menu.addAction("Copy Selected", [this, selectedIndexes]() {
                clipboardFilePath = fileModel->filePath(proxyModel->mapToSource(selectedIndexes.first()));
                clipboardIsCut = false;
                logMessage(QString("[INFO] Copied %1 item(s)").arg(selectedIndexes.size()));
            });
            menu.addAction("Cut Selected", [this, selectedIndexes]() {
                clipboardFilePath = fileModel->filePath(proxyModel->mapToSource(selectedIndexes.first()));
                clipboardIsCut = true;
                logMessage(QString("[INFO] Cut %1 item(s)").arg(selectedIndexes.size()));
            });
            if (!clipboardFilePath.isEmpty()) {
                menu.addAction("Paste Here", [this, path, isDir]() {
                    QString destDir = isDir ? path : QFileInfo(path).absolutePath();
                    QString dest = destDir + "/" + QFileInfo(clipboardFilePath).fileName();
                    int copyCount = 1;
                    while (QFileInfo::exists(dest) && (!clipboardIsCut || dest != clipboardFilePath)) {
                        QString base = QFileInfo(clipboardFilePath).completeBaseName();
                        QString suf = QFileInfo(clipboardFilePath).suffix();
                        if (!suf.isEmpty()) suf = "." + suf;
                        dest = destDir + "/" + base + (copyCount == 1 ? "_copy" : "_copy" + QString::number(copyCount)) + suf;
                        copyCount++;
                    }
                    if (clipboardIsCut) {
                        QFile::rename(clipboardFilePath, dest);
                        clipboardFilePath = "";
                    } else {
                        QFile::copy(clipboardFilePath, dest);
                    }
                });
            }

            // Batch conversion for all recognised formats.
            QStringList filePaths;
            for (auto& idx : selectedIndexes) {
                QString p = fileModel->filePath(proxyModel->mapToSource(idx));
                if (!QFileInfo(p).isDir()) filePaths << p;
            }
            if (!filePaths.isEmpty()) {
                QMenu* batchMenu = menu.addMenu("Batch Conversion");
                auto addBatch = [&](const QString& label, const QString& newExt, const QStringList& validExts) {
                    int count = 0;
                    for (const QString& p : filePaths) {
                        if (validExts.contains(QFileInfo(p).suffix().toLower())) ++count;
                    }
                    if (count == 0) return;
                    batchMenu->addAction(QString("%1 %2 file(s) to %3").arg(label).arg(count).arg(newExt.toUpper()), [this, filePaths, newExt, validExts]() {
                        QString outDir = QFileDialog::getExistingDirectory(this, "Select output folder", QFileInfo(filePaths.first()).absolutePath());
                        if (outDir.isEmpty()) return;
                        int done = 0, failed = 0;
                        for (const QString& src : filePaths) {
                            if (!validExts.contains(QFileInfo(src).suffix().toLower())) continue;
                            QString dst = outDir + "/" + QFileInfo(src).completeBaseName() + "." + newExt;
                            QString cmd;
                            QStringList args;
                            QString ext = QFileInfo(src).suffix().toLower();
                            if (newExt == "obj") {
                                args << "mef" << "export" << src << "-o" << dst;
                            } else if (newExt == "txt") {
                                args << "mef" << "to-text" << src << "-o" << dst;
                            } else if (newExt == "wav") {
                                args << "wav" << "convert" << src << "-o" << dst;
                            } else if (newExt == "bef") {
                                args << "iff" << "convert" << src << outDir;
                            } else if (newExt == "tga" || newExt == "png" || newExt == "spr" || newExt == "tex") {
                                QString sub = (newExt == "tga") ? "to-tga" : (newExt == "png") ? "to-png" : "to-spr";
                                args << "tex" << sub << src << "-o" << dst;
                            }
                            if (args.isEmpty()) { ++failed; continue; }
                            QProcess proc;
                            proc.setProgram(qApp->applicationFilePath());
                            proc.setArguments(args);
                            proc.start();
                            proc.waitForFinished(-1);
                            if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                                ++done;
                            } else {
                                ++failed;
                                logMessage("[ERROR] Batch convert failed: " + src + " -> " + dst + "\n" + proc.readAllStandardError());
                            }
                        }
                        logMessage(QString("[INFO] Batch conversion complete: %1 succeeded, %2 failed").arg(done).arg(failed));
                        QMessageBox::information(this, "Batch Conversion", QString("Converted %1 file(s) to %2\n%3 succeeded, %4 failed").arg(done + failed).arg(newExt.toUpper()).arg(done).arg(failed));
                    });
                };
                addBatch("Convert", "tga",  {"tex", "spr", "pic"});
                addBatch("Convert", "png",  {"tex", "spr", "pic"});
                addBatch("Convert", "spr",  {"tex", "spr", "pic", "png", "tga", "bmp", "jpg", "jpeg"});
                addBatch("Convert", "tex",  {"png", "tga", "bmp", "jpg", "jpeg", "spr", "pic"});
                addBatch("Convert", "obj",  {"mef", "mex"});
                addBatch("Convert", "txt",  {"mef", "mex"});
                addBatch("Convert", "wav",  {"wav"});
                addBatch("Convert", "bef",  {"iff", "bff"});
            }

            menu.exec(treeView->mapToGlobal(pos));
            return;
        }


        // Single item context menu
        if (isDir) {
            QDir d(path);
            if (!d.entryList({"*.iff", "*.IFF"}, QDir::Files).isEmpty()) {
                menu.addAction("Batch Convert IFF -> BEF", [this, path]() {
                    QString outDir = QFileDialog::getExistingDirectory(this, "Select Output Directory", path);
                    if (outDir.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "convert", path, outDir});
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Batch converted animations in directory to " + outDir);
                        QMessageBox::information(this, "Success", "Batch converted animation files.");
                    } else {
                        logMessage("[ERROR] IFF batch convert failed:\n" + proc.readAllStandardError());
                        QMessageBox::critical(this, "Error", "Failed to batch convert IFF files:\n" + proc.readAllStandardError());
                    }
                });

                menu.addAction("Decompile IFF -> text + per-anim IFFs", [this, path]() {
                    QString outDir = QFileDialog::getExistingDirectory(this, "Select Output Directory", path);
                    if (outDir.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "decompile", path, outDir});
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Decompiled IFF into " + outDir);
                        QMessageBox::information(this, "Success", "Decompiled IFF to text + per-anim IFFs.");
                    } else {
                        QString err = proc.readAllStandardError();
                        logMessage("[ERROR] IFF decompile failed:\n" + err);
                        QMessageBox::critical(this, "Error", "Failed to decompile IFF:\n" + err);
                    }
                });
                menu.addSeparator();
            }
            // If the folder contains a batch of .BEF scripts, offer to pack them
            // back into a single IFF (the inverse of convert).
            if (!d.entryList({"*.bef", "*.BEF"}, QDir::Files).isEmpty()) {
                menu.addAction("Create IFF from .BEF scripts", [this, path]() {
                    QString outIff = QFileDialog::getSaveFileName(this, "Save IFF",
                        QFileInfo(path).absoluteFilePath() + ".iff", "IFF Skeletal Animation (*.iff)");
                    if (outIff.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "create", path, outIff});
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Wrote IFF " + outIff);
                        QMessageBox::information(this, "Success", "Created IFF file.");
                    } else {
                        QString err = proc.readAllStandardError();
                        logMessage("[ERROR] IFF create failed:\n" + err);
                        QMessageBox::critical(this, "Error", "Failed to create IFF:\n" + err);
                    }
                });
                menu.addAction("Generate Anims.qsc for .BEF scripts", [this, path]() {
                    QString outQsc = QFileDialog::getSaveFileName(this, "Save Anims.qsc",
                        QFileInfo(path).absoluteFilePath() + "/Anims.qsc", "QScript (*.qsc)");
                    if (outQsc.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "emit-qsc", path, outQsc});
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Wrote " + outQsc);
                    } else {
                        QString err = proc.readAllStandardError();
                        logMessage("[ERROR] emit-qsc failed:\n" + err);
                        QMessageBox::critical(this, "Error", "Failed to write Anims.qsc:\n" + err);
                    }
                });
                menu.addSeparator();
            }

            QString folderName = QFileInfo(path).fileName();
            menu.addAction("Pack to Archive", [this, path, folderName]() {
                // Auto-Save: if a sibling .RES exists with the same
                // name as the folder (level1/textures/ -> level1.res),
                // pack to it directly.  Otherwise ask the user.
                QString defaultOut = path + "/" + folderName + ".res";
                QString outRes;
                if (QFileInfo::exists(defaultOut)) {
                    outRes = defaultOut;
                } else {
                    outRes = QFileDialog::getSaveFileName(this, "Save Resource Archive",
                        defaultOut, "Resource Archive (*.res)");
                    if (outRes.isEmpty()) return;
                }

                QString prefix = folderName + "/";

                QProcess proc;
                proc.setProgram(qApp->applicationFilePath());
                QStringList args;
                args << "res" << "pack" << path << outRes;
                if (!prefix.isEmpty()) {
                    args << "--prefix" << prefix;
                }
                proc.setArguments(args);
                proc.start();
                proc.waitForFinished(-1);
                if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                    logMessage("[SUCCESS] Packed folder to " + outRes);
                    QMessageBox::information(this, "Packed", "Successfully packed to:\n" + outRes);
                } else {
                    QString err = proc.readAllStandardError();
                    logMessage("[ERROR] Failed to pack folder to " + outRes + "\n" + err);
                    QMessageBox::critical(this, "Pack Failed", "Error packing folder:\n" + err);
                }
            });
            menu.addSeparator();
        }
        if (!isDir) {
            menu.addAction("Open in Native App", [path]() { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });

            QMenu* viewMenu = menu.addMenu("View As");
            viewMenu->addAction("Auto",      [this, path]() { loadFile(path, 0); });
            viewMenu->addAction("Text",      [this, path]() { loadFile(path, 1); });
            viewMenu->addAction("Hex",       [this, path]() { loadFile(path, 2); });
            viewMenu->addAction("Image",     [this, path]() { loadFile(path, 3); });
            viewMenu->addAction("Audio",     [this, path]() { loadFile(path, 4); });
            viewMenu->addAction("Animation", [this, path]() { loadFile(path, 5); });
            viewMenu->addAction("3D",        [this, path]() { loadFile(path, 6); });
            menu.addSeparator();

            if (ext == "iff" || ext == "bff") {
                menu.addAction("Play Animation", [this, path]() {
                    playAnimationForFile(path);
                });
                // Apply this IFF animation to a chosen MEF model in the
                // current level.  Scans the configured models directory
                // for *.mef, lets the user pick one, then loads the MEF
                // and the IFF together so the 3D viewer shows the
                // animation playing on that model.
                menu.addAction("Apply Animation on Model...", [this, path]() {
                    applyIffOnModel(path);
                });
                menu.addSeparator();
                menu.addAction("Convert to BEF", [this, path]() {
                    QString dir = QFileInfo(path).absolutePath();
                    QString tempDir = QDir::tempPath() + "/igi1conv_iff_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                    QDir().mkpath(tempDir + "/Input");
                    QFile::copy(path, tempDir + "/Input/" + QFileInfo(path).fileName());

                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "convert", tempDir + "/Input", tempDir + "/Converted"});
                    proc.start();
                    proc.waitForFinished(-1);

                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        QStringList filters; filters << "*.BEF" << "*.bef";
                        QDir out(tempDir + "/Converted");
                        for (QString f : out.entryList(filters, QDir::Files)) {
                            QFile::copy(tempDir + "/Converted/" + f, dir + "/" + f);
                        }
                        logMessage("[SUCCESS] Converted IFF to BEF in " + dir);
                        QMessageBox::information(this, "Success", "Converted IFF file.");
                    } else {
                        logMessage("[ERROR] IFF convert failed:\n" + proc.readAllStandardError());
                        QMessageBox::critical(this, "Error", "Failed to convert IFF file:\n" + proc.readAllStandardError());
                    }
                    QDir(tempDir).removeRecursively();
                });

                menu.addAction("Decompile to text + per-anim IFFs", [this, path]() {
                    QString outDir = QFileDialog::getExistingDirectory(this, "Select Output Directory", QFileInfo(path).absolutePath());
                    if (outDir.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments({"iff", "decompile", path, outDir});
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Decompiled " + path + " into " + outDir);
                        QMessageBox::information(this, "Success", "Decompiled IFF file to text + per-anim IFFs.");
                    } else {
                        QString err = proc.readAllStandardError();
                        logMessage("[ERROR] IFF decompile failed:\n" + err);
                        QMessageBox::critical(this, "Error", "Failed to decompile IFF:\n" + err);
                    }
                });

                menu.addAction("Export Animation as GIF...", [this, path]() {
                    QString outPath = QFileDialog::getSaveFileName(this, "Export GIF", QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".gif", "GIF Image (*.gif)");
                    if (!outPath.isEmpty()) {
                        // Spawn the CLI's native headless renderer so the GUI's
                        // OpenGL viewport doesn't have to be visible.  This is
                        // the same code path as `igi1conv iff export-gif`.
                        int w = modelViewer->width()  > 0 ? modelViewer->width()  : 640;
                        int h = modelViewer->height() > 0 ? modelViewer->height() : 480;
                        // Indeterminate progress dialog shown while the
                        // CLI runs in the background.  The CLI's stdout
                        // is captured so we can update the dialog with
                        // per-frame progress if it emits it.
                        QProgressDialog progress(
                            QString("Exporting GIF from %1...").arg(QFileInfo(path).fileName()),
                            "Cancel", 0, 0, this);
                        progress.setWindowModality(Qt::WindowModal);
                        progress.setMinimumDuration(0);
                        progress.setAutoClose(false);
                        progress.show();

                        QProcess proc;
                        proc.setProgram(qApp->applicationFilePath());
                        proc.setArguments({"iff", "export-gif", path, outPath,
                                          QString::number(w), QString::number(h), "15"});
                        proc.start();
                        // Pump events while waiting so the dialog repaints
                        // and the Cancel button stays responsive.  The
                        // CLI doesn't emit per-frame progress yet, so the
                        // dialog is indeterminate (0/0).  When Cancel is
                        // pressed, kill the CLI process.
                        while (proc.state() != QProcess::NotRunning) {
                            if (progress.wasCanceled()) {
                                proc.kill();
                                proc.waitForFinished(1000);
                                break;
                            }
                            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                        }
                        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                            progress.close();
                            logMessage("[SUCCESS] GIF written to " + outPath);
                            QMessageBox::information(this, "Success",
                                "GIF written successfully to:\n" + outPath);
                        } else {
                            progress.close();
                            QString err = proc.readAllStandardError();
                            logMessage("[ERROR] GIF export failed:\n" + err);
                            QMessageBox::critical(this, "Error", "GIF export failed:\n" + err);
                        }
                    }
                });
            }

            // Play Animation for .mef files whose model ID falls within
            // the 000_00_0 – 030_00_0 range.
            if (ext == "mef" || ext == "mex") {
                QString baseName = QFileInfo(path).completeBaseName();
                if (baseName >= "000_00_0" && baseName <= "030_00_0") {
                    menu.addAction("Play Animation", [this, path]() {
                        playAnimationForFile(path);
                    });
                }
            }
        }

        menu.addSeparator();
        menu.addAction("Rename...", [this, path]() {
            bool ok;
            QString newName = QInputDialog::getText(this, "Rename File", "New name:", QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
            if (ok && !newName.isEmpty()) {
                QString newPath = QFileInfo(path).absolutePath() + "/" + newName;
                if (QFileInfo(path).isDir()) {
                    QDir().rename(path, newPath);
                } else {
                    QFile::rename(path, newPath);
                }
            }
        });
        menu.addAction("Delete", [this, path]() {
            if (QMessageBox::question(this, "Delete", "Are you sure you want to delete " + QFileInfo(path).fileName() + "?") == QMessageBox::Yes) {
                if (QFileInfo(path).isDir()) {
                    QDir(path).removeRecursively();
                } else {
                    QFile(path).remove();
                }
            }
        });
        menu.addAction("Cut", [this, path]() { clipboardFilePath = path; clipboardIsCut = true; });
        menu.addAction("Copy", [this, path]() { clipboardFilePath = path; clipboardIsCut = false; });
        if (!clipboardFilePath.isEmpty()) {
            menu.addAction("Paste Here", [this, path, isDir]() {
                QString destDir = isDir ? path : QFileInfo(path).absolutePath();
                QString dest = destDir + "/" + QFileInfo(clipboardFilePath).fileName();
                
                int copyCount = 1;
                while (QFileInfo::exists(dest) && (!clipboardIsCut || dest != clipboardFilePath)) {
                    QString base = QFileInfo(clipboardFilePath).completeBaseName();
                    QString ext = QFileInfo(clipboardFilePath).suffix();
                    if (!ext.isEmpty()) ext = "." + ext;
                    if (copyCount == 1) dest = destDir + "/" + base + "_copy" + ext;
                    else dest = destDir + "/" + base + "_copy" + QString::number(copyCount) + ext;
                    copyCount++;
                }

                if (QFileInfo(clipboardFilePath).isDir()) {
                    QProcess::execute("cmd", QStringList() << "/c" << "xcopy" << QDir::toNativeSeparators(clipboardFilePath) << QDir::toNativeSeparators(dest) << "/E" << "/I" << "/H" << "/Y");
                } else {
                    QFile::copy(clipboardFilePath, dest);
                }
                
                if (clipboardIsCut) {
                    if (QFileInfo(clipboardFilePath).isDir()) QDir(clipboardFilePath).removeRecursively();
                    else QFile(clipboardFilePath).remove();
                    clipboardFilePath = "";
                }
            });
        }
        menu.addSeparator();

        if (isDir) {
            bool hasMefs = false;
            QDir dir(path);
            if (!dir.entryList(QStringList() << "*.mef", QDir::Files).isEmpty()) hasMefs = true;
            if (hasMefs) {
                menu.addSeparator();
                menu.addAction("Apply Textures All (in Folder)", [this, path]() {
                    currentFile = path;
                    executeCommand("mef apply-tex-all");
                });
                menu.addAction("Export All (in Folder)", [this, path]() {
                    currentFile = path;
                    executeCommand("mef export-bundle-all");
                });
            }
            // Batch wav conversion: if this folder (recursively) contains
            // any *.wav files, expose a submenu with one entry per output
            // format.  The user picks the destination folder at click
            // time.  We do a recursive scan instead of a flat one so a
            // right-click on a parent folder that only contains wav
            // files in subfolders still surfaces the option.
            int wavCount = countWavFilesRec(path);
            if (wavCount > 0) {
                menu.addSeparator();
                QMenu* batchWavMenu = menu.addMenu(
                    QString("Batch Convert %1 .wav file(s) in Folder").arg(wavCount));
                batchWavMenu->addAction("Convert all to .wav (Windows PCM)", [this, path]() {
                    convertIgiWavDir(path);
                });
            }
        } else if (ext == "tex" || ext == "spr" || ext == "pic") {
            menu.addAction("Convert to PNG", [this, path]() { loadFile(path); executeCommand("tex to-png"); });
            menu.addAction("Convert to TGA", [this, path]() { loadFile(path); executeCommand("tex to-tga"); });
            menu.addAction("Replace Texture", [this, path]() {
                QString newTex = QFileDialog::getOpenFileName(this, "Select Replacement Texture", QFileInfo(path).absolutePath(), "Texture Files (*.tex)");
                if (!newTex.isEmpty()) {
                    QFile::remove(path);
                    if (QFile::copy(newTex, path)) {
                        logMessage("[SUCCESS] Replaced texture: " + QFileInfo(path).fileName());
                        QMessageBox::information(this, "Replaced", "Successfully replaced texture.");
                        if (globalAutoSaveRes) {
                            QDir parentDir = QFileInfo(path).absoluteDir();
                            QString folderName = parentDir.dirName();
                            QString parentOfParent = parentDir.absolutePath() + "/..";
                            QString resPath = QDir::cleanPath(parentOfParent + "/" + folderName + ".res");
                            if (QFileInfo::exists(resPath)) {
                                logMessage("[INFO] Auto Saving RES: " + resPath);
                                QProcess proc;
                                proc.setProgram(qApp->applicationFilePath());
                                proc.setArguments(QStringList() << "res" << "pack" << parentDir.absolutePath() << resPath << "--prefix" << folderName + "/");
                                proc.start();
                                proc.waitForFinished(-1);
                                if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                                    logMessage("[SUCCESS] Auto Packed RES archive: " + resPath);
                                } else {
                                    logMessage("[ERROR] Auto Save Failed to pack RES: " + resPath + "\n" + proc.readAllStandardError());
                                }
                            }
                        }
                    } else {
                        logMessage("[ERROR] Failed to replace texture: " + path);
                        QMessageBox::critical(this, "Error", "Failed to replace texture.");
                    }
                }
            });
            menu.addAction("Info",           [this, path]() { loadFile(path); executeCommand("tex info"); });
            menu.addAction("Decode Batch",   [this, path]() { loadFile(path); executeCommand("tex decode-batch"); });
        } else if (ext == "png" || ext == "tga" || ext == "bmp" || ext == "jpg" || ext == "jpeg") {
            // .tex (RGB565, the default 16-bit format) and .spr
            // (ARGB8888, full quality with alpha) are both LOOP v11
            // containers - same header, different pixel mode.  Both
            // go through the shared TEX_WriteLOOP encoder so the
            // output is bit-identical to what `igi1conv tex to-spr`
            // produces.
            menu.addAction("Convert to TEX", [this, path]() {
                QString newPath = path.left(path.lastIndexOf('.')) + ".tex";
                QImage img = loadImageSafe(path);
                if (img.isNull()) {
                    QString err = QString("Could not load image: %1").arg(path);
                    logMessage("[ERROR] " + err);
                    QMessageBox::critical(this, "Convert Failed", err);
                    return;
                }
                if (imageEditor->saveAsTex(img, newPath, /*mode=*/2)) {
                    logMessage("[INFO] Converted image to TEX: " + newPath);
                    QMessageBox::information(this, "Success", "Converted to " + newPath);
                } else {
                    QString err = QString("Failed to convert image to TEX: %1 -> %2")
                                       .arg(path, newPath);
                    logMessage("[ERROR] " + err);
                    QMessageBox::critical(this, "Convert Failed", err);
                }
            });
            menu.addAction("Convert to SPR", [this, path]() {
                QString newPath = path.left(path.lastIndexOf('.')) + ".spr";
                QImage img = loadImageSafe(path);
                if (img.isNull()) {
                    QString err = QString("Could not load image: %1").arg(path);
                    logMessage("[ERROR] " + err);
                    QMessageBox::critical(this, "Convert Failed", err);
                    return;
                }
                if (imageEditor->saveAsTex(img, newPath, /*mode=*/3)) {
                    logMessage("[INFO] Converted image to SPR: " + newPath);
                    QMessageBox::information(this, "Success", "Converted to " + newPath);
                } else {
                    QString err = QString("Failed to convert image to SPR: %1 -> %2")
                                       .arg(path, newPath);
                    logMessage("[ERROR] " + err);
                    QMessageBox::critical(this, "Convert Failed", err);
                }
            });
        } else if (ext == "qsc" || ext == "qas") {
            menu.addAction("Compile",        [this, path]() { loadFile(path); executeCommand("qsc compile"); });
            menu.addAction("Validate",       [this, path]() { loadFile(path); executeCommand("qsc validate"); });
            if (globalAnimationModeEnabled) {
                menu.addAction("Use as Animation objects.qsc", [this, path]() {
                    globalObjectsQscPath = path;
                    QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
                    QSettings(iniPath, QSettings::IniFormat).setValue("ObjectsQscPath", path);
                    logMessage("[INFO] Animation source set: " + path);
                    loadAnimationSetFromQsc();
                });
            }
        } else if (ext == "qvm") {
            menu.addAction("Decompile",      [this, path]() { loadFile(path); executeCommand("qvm decompile"); });
            menu.addAction("Disassemble",    [this, path]() { loadFile(path); executeCommand("qvm disasm"); });
            menu.addAction("Info",           [this, path]() { loadFile(path); executeCommand("qvm info"); });
        } else if (ext == "mef" || ext == "mex") {
            bool isBinary = true;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray magic = f.read(4);
                if (magic != "ILFF") {
                    isBinary = false;
                }
                f.close();
            }

            QString cmdPrefix = ext;

            if (isBinary) {
                // Play Animation only applies to HumanSoldier AI models -
                // these are model ids in the 000_00_0..030_00_0 range (the
                // same range applyIffOnModel's picker filters to). Building/
                // prop meshes outside that range have no bone animation set
                // and showing the action for them is misleading.
                QString modelId = QFileInfo(path).completeBaseName();
                if (modelId >= "000_00_0" && modelId <= "030_00_0") {
                    menu.addAction("Play Animation", [this, path]() {
                        playAnimationForFile(path);
                    });
                    menu.addSeparator();
                }

                QMenu* infoMenu = menu.addMenu("Details");
                infoMenu->addAction("Info", [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " info"); });
                infoMenu->addAction("Dump", [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " dump"); });

                QMenu* exportMenu = menu.addMenu("Export");
                exportMenu->addAction("Export to Obj",       [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " export-bundle"); });
                if (ext == "mef") {
                    exportMenu->addAction("Export to Mef(Text)",      [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " to-text"); });
                    exportMenu->addAction("Build Rigid Model", [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " build-rigid"); });
                } else {
                    exportMenu->addAction("Export to Mex(Text)",      [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " to-text"); });
                }

                if (ext == "mef") {

                    // Textures submenu
                    QMenu* texMenu = menu.addMenu("Textures");
                    texMenu->addAction("Apply Textures", [this, path]() {
                        currentFile = path;
                        executeCommand("mef apply-tex");
                        QString baseName = QFileInfo(path).completeBaseName();
                        QString tempDir = globalCacheDir + "/bundle/" + baseName + "/";
                        if (QDir(tempDir).exists()) {
                            logMessage("[INFO] Extracted textures to temp folder. Loading native MEF with textures: " + path);
                            viewModeCombo->setCurrentIndex(6);
                            modelViewer->loadModel(path);
                            modelViewer->show();
                        }
                    });
                    texMenu->addAction("Apply Textures (All)", [this, path]() {
                        currentFile = QFileInfo(path).absolutePath();
                        executeCommand("mef apply-tex-all");
                    });
                    texMenu->addAction("Apply Lightmap", [this, path]() {
                        applyLightmapOnModel(path);
                    });
                    texMenu->addAction("Map Textures", [this, path]() {
                        QString baseName = QFileInfo(path).completeBaseName();
                        if (globalLevelDatPath.isEmpty()) {
                            logMessage("[ERROR] No level DAT set. Go to Settings > Level to set one.");
                            return;
                        }
                        QProcess proc;
                        proc.setProgram(qApp->applicationFilePath());
                        proc.setArguments(QStringList() << "dat" << "export" << globalLevelDatPath);
                        proc.start();
                        proc.waitForFinished(10000);
                        QString datOut = proc.readAllStandardOutput();

                        QString report = QString("=== Texture Map: %1 ===\n").arg(baseName);
                        int modelIdx = datOut.indexOf(QString("\"modelName\": \"%1\"").arg(baseName), 0, Qt::CaseInsensitive);
                        if (modelIdx != -1) {
                            int texStart = datOut.indexOf("\"textures\": [", modelIdx);
                            if (texStart != -1) {
                                int texEnd = datOut.indexOf("]", texStart);
                                QString texArr = datOut.mid(texStart + 12, texEnd - texStart - 12);
                                report += "Textures mapped in DAT:\n" + texArr.replace("\"", "").trimmed() + "\n";
                            } else {
                                report += "No textures found in DAT for this model.\n";
                            }
                        } else {
                            report += "(No texture mapping found in DAT for this model)\n";
                        }
                        QProcess mefProc;
                        mefProc.setProgram(qApp->applicationFilePath());
                        mefProc.setArguments(QStringList() << "mef" << "dump" << path);
                        mefProc.start();
                        mefProc.waitForFinished(5000);
                        QString mefOut = mefProc.readAllStandardOutput();
                        report += "\n=== MEF Materials & Attachments ===\n";
                        for (const QString& line : mefOut.split('\n')) {
                            if (line.startsWith("material") || line.startsWith("ATTA") || line.startsWith("attachment"))
                                report += line.trimmed() + "\n";
                        }
                        logMessage(report);
                    });
                }
            } else {
                menu.addAction("Compile to Binary", [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " compile-text"); });
                QMenu* exportMenu = menu.addMenu("Export");
                exportMenu->addAction("Export to Obj", [this, path, cmdPrefix]() { loadFile(path); executeCommand(cmdPrefix + " export"); });
            }
        } else if (ext == "res") {
            menu.addAction("Extract", [this, path]() { loadFile(path); executeCommand("res extract"); });
            menu.addAction("List",    [this, path]() { loadFile(path); executeCommand("res list"); });
            menu.addAction("Unpack",  [this, path]() { loadFile(path); executeCommand("res unpack"); });
        } else if (ext == "mtp") {
            menu.addAction("Convert",       [this, path]() { loadFile(path); executeCommand("mtp to-dat"); });
            menu.addAction("Dump",          [this, path]() { loadFile(path); executeCommand("mtp dump"); });
            menu.addAction("Info",          [this, path]() { loadFile(path); executeCommand("mtp info"); });
        } else if (ext == "dat") {
            // Detect the AI navigation graph file.  IGI 1 stores the
            // AI graph in a file named "graph.dat" (sometimes "graph1.dat"
            // or a level-specific variant).  This is a different format
            // from the level DAT (which lists model/texture bindings) and
            // requires a dedicated graph parser, not dat_parser.
            QString datBase = QFileInfo(path).completeBaseName().toLower();
            bool isGraphDat = (datBase == "graph" || datBase == "graph1" ||
                                datBase == "graph2" || datBase == "graph3" ||
                                datBase.startsWith("graph"));
            if (isGraphDat) {
                // graph.dat - dedicated context menu.  No "View Graph in
                // 3D" action — the graph is opened automatically by the
                // normal text-viewer flow (loadFile routes .dat to the
                // 3D viewer because of the "graph" name heuristic in
                // loadFile()), so an explicit 3D-view action is
                // redundant.  The two structured export actions below
                // give the user access to the graph in a portable
                // JSON / Markdown form.
                menu.addAction("Export to JSON", [this, path]() {
                    QString out = QFileDialog::getSaveFileName(this, "Export Graph to JSON",
                        path + ".json", "JSON Files (*.json)");
                    if (out.isEmpty()) return;
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments(QStringList() << "graph" << "export" << path << "--out" << out);
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Exported graph to JSON: " + out);
                    } else {
                        logMessage("[ERROR] graph export failed: " + proc.readAllStandardError());
                    }
                });
                menu.addAction("Export to Table (.md)", [this, path]() {
                    QString out = QFileDialog::getSaveFileName(this, "Export Graph Table",
                        path + ".md", "Markdown Files (*.md)");
                    if (out.isEmpty()) return;
                    // Use the new "graph table" CLI subcommand which
                    // emits a structured Markdown table with one row
                    // per node, one row per link, and every field
                    // (XYZ, gamma, radius, material, criteria, link1,
                    // link2, legacy link targets/types, link type).
                    QProcess proc;
                    proc.setProgram(qApp->applicationFilePath());
                    proc.setArguments(QStringList() << "graph" << "table" << path << "--out" << out);
                    proc.start();
                    proc.waitForFinished(-1);
                    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
                        logMessage("[SUCCESS] Exported graph table: " + out);
                    } else {
                        logMessage("[ERROR] graph table failed: " + proc.readAllStandardError());
                    }
                });
                menu.addAction("Info", [this, path]() { loadFile(path); executeCommand("graph info"); });
                menu.addAction("Dump", [this, path]() { loadFile(path); executeCommand("graph dump"); });
            } else {
                // Regular level DAT (model/texture bindings)
                menu.addAction("Convert",       [this, path]() { loadFile(path); executeCommand("dat to-mtp"); });
                menu.addAction("Export",        [this, path]() { loadFile(path); executeCommand("dat export"); });
                menu.addAction("Info",          [this, path]() { loadFile(path); executeCommand("dat info"); });
            }
        } else if (ext == "fnt") {
            menu.addAction("Export PNG", [this, path]() { loadFile(path); executeCommand("fnt export"); });
            menu.addAction("Info",       [this, path]() { loadFile(path); executeCommand("fnt info"); });
        } else if (ext == "iff") {
            menu.addAction("Info",       [this, path]() { loadFile(path); executeCommand("iff info"); });
            if (globalAnimationModeEnabled) {
                menu.addAction("Pre-Extract to Animation Cache", [this, path]() {
                    if (globalAnimsCacheDir.isEmpty()) {
                        logMessage("[WARN] Animation cache folder not set. Use Settings > Animation.");
                        return;
                    }
                    QDir().mkpath(globalAnimsCacheDir);
                    QString dst = globalAnimsCacheDir + "/" + QFileInfo(path).fileName();
                    if (QFile::exists(dst)) QFile::remove(dst);
                    if (QFile::copy(path, dst)) {
                        logMessage("[INFO] Pre-extracted " + path + " -> " + dst);
                    } else {
                        logMessage("[ERROR] Failed to copy " + path + " -> " + dst);
                    }
                });
            }
        } else if (ext == "wav") {
            // IGI audio (.wav files are the proprietary ILSF container
            // in the game directory).  This build is single-binary / zero
            // external DLL, so the only output format is standard PCM
            // .wav.  Three actions:
            //   1. Play in default media player - lands the decoded
            //      .wav in the audio cache (SHA-256 keyed) so re-plays
            //      are instant, then hands the URL to the OS.
            //   2. Convert to .wav (Windows PCM)... - opens a Save As
            //      dialog; the file lands wherever the user picks.
            //   3. Info - runs `wav info` in the text viewer.
            menu.addAction("Play in default media player", [this, path]() {
                playIgiWav(path);
            });
            menu.addSeparator();
            menu.addAction("Convert to .wav (Windows PCM)...", [this, path]() {
                convertIgiWav(path);
            });
            menu.addSeparator();
            menu.addAction("Info", [this, path]() { loadFile(path); executeCommand("wav info"); });
        } else if (ext == "olm") {
            menu.addAction("Info / Dump", [this, path]() { loadFile(path); executeCommand("olm info"); });
            QMenu* exportMenu = menu.addMenu("Export");
            exportMenu->addAction("Export to PNG", [this, path]() { loadFile(path); executeCommand("olm to-png"); });
            exportMenu->addAction("Export to TGA", [this, path]() { loadFile(path); executeCommand("olm to-tga"); });
        }
        menu.exec(treeView->mapToGlobal(pos));
    }

    void loadFile(const QString& path, int overrideViewMode = 0) {
        currentFile = path;
        QFileInfo info(path);
        currentExt = info.suffix().toLower();

        hideAllViewers();

        int mode = overrideViewMode;
        if (mode == 0) {
            if (currentExt == "png" || currentExt == "jpg" || currentExt == "jpeg" || currentExt == "bmp" || currentExt == "tex" || currentExt == "spr" || currentExt == "pic" || currentExt == "tga" || currentExt == "olm") {
                mode = 3; // Image
            } else if (currentExt == "mef" || currentExt == "mex") {
                bool isBinary = true;
                QFile f(path);
                if (f.open(QIODevice::ReadOnly)) {
                    QByteArray magic = f.read(4);
                    if (magic != "ILFF") isBinary = false;
                    f.close();
                }
                if (isBinary) {
                    mode = 6; // 3D
                } else {
                    mode = 1; // Text
                }
            } else if (currentExt == "obj") {
                mode = 6; // 3D
            } else if (currentExt == "iff") {
                mode = 5; // Animation (unified IFF/MEF animation mode)
            } else if (currentExt == "wav") {
                // IGI audio: always route to the in-process Audio mode.
                mode = 4; // Audio
            } else if (currentExt == "qsc" || currentExt == "txt" || currentExt == "json" || currentExt == "md" || currentExt == "h" || currentExt == "cpp" || currentExt == "dat" || currentExt == "qvm") {
                if (currentExt == "dat" && info.fileName().toLower().contains("graph")) {
                    mode = 6; // 3D
                } else {
                    mode = 1; // Text
                }
            } else {
                mode = 2; // Hex
            }
            viewModeCombo->blockSignals(true);
            viewModeCombo->setCurrentIndex(mode);
            viewModeCombo->blockSignals(false);
        }

        if (mode == 6 && currentExt == "mef") {
            // We directly use native MEF viewing. The textures will be found in temp folder if bundled.
        }

        if (mode == 1) { // Text
            QString loadPath = path;
            if (currentExt == "qvm") {
                consoleEdit->append("[INFO] Decompiling QVM to QSC automatically for viewing...");
                loadPath = QDir::tempPath() + "/igi_temp.qsc";
                QString cmd = qApp->applicationFilePath();
                QProcess::execute(cmd, QStringList() << "qvm" << "decompile" << path << "-o" << loadPath);
            }
            QFile file(loadPath);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                viewerEdit->setPlainText(QString::fromUtf8(file.readAll()));
            }
            viewerEdit->isHexMode = false;
            viewerEdit->setReadOnly(false);
            viewerEdit->show();
        } else if (mode == 2) { // Hex
            // If this is a graph*.dat file, use the specialised
            // GraphHexEditor (16-byte-wide hex view + graph info
            // panel: Signature, Max Nodes, Data-Type, Node #, Graph
            // Data, plus an Item dropdown matching the legacy IGI
            // Graph Editor's `graphHexItemsDD_SelectedIndexChanged`).
            // For every other file, fall back to the plain hex dump
            // in viewerEdit.
            QString baseName = QFileInfo(path).completeBaseName().toLower();
            bool isGraphHex = (baseName == "graph" || baseName == "graph1" ||
                               baseName == "graph2" || baseName == "graph3" ||
                               baseName.startsWith("graph"));
            if (isGraphHex && graphHexEditor) {
                graphHexEditor->loadFile(path);
                graphHexEditor->show();
                viewerEdit->hide();
            } else {
                if (graphHexEditor) graphHexEditor->hide();
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {
                    QByteArray data = file.readAll();
                    QString hexView;
                    int lines = qMin(data.size() / 16 + 1, 4000);
                    for (int i = 0; i < lines; ++i) {
                        int offset = i * 16;
                        hexView += QString("%1: ").arg(offset, 8, 16, QChar('0')).toUpper();
                        for (int n = 0; n < 16; n++) {
                            if (offset + n < data.size()) {
                                hexView += QString("%1 ").arg((quint8)data[offset + n], 2, 16, QChar('0')).toUpper();
                            } else {
                                hexView += "   ";
                            }
                        }
                        hexView += "| ";
                        for (int n = 0; n < 16; n++) {
                            if (offset + n < data.size()) {
                                char c = data[offset + n];
                                hexView += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
                            } else {
                                hexView += " ";
                            }
                        }
                        hexView += "\n";
                    }
                    if (data.size() > 4000 * 16) hexView += "\n... (Truncated) ...";
                    viewerEdit->setPlainText(hexView);
                }
                viewerEdit->isHexMode = true;
                viewerEdit->setReadOnly(false);
                viewerEdit->show();
            }
        } else if (mode == 3) { // Image
            if (currentExt == "olm") {
                OLMFile olm = ParseOlm(path.toStdString());
                if (olm.valid && !olm.pixels.empty()) {
                    QImage qimg(olm.layer.pixel_width, olm.layer.pixel_height, QImage::Format_ARGB32);
                    for (uint32_t y = 0; y < olm.layer.pixel_height; ++y) {
                        for (uint32_t x = 0; x < olm.layer.pixel_width; ++x) {
                            size_t i = y * olm.layer.pixel_width + x;
                            qimg.setPixelColor(x, y, QColor(olm.pixels[i].r, olm.pixels[i].g, olm.pixels[i].b, olm.pixels[i].a));
                        }
                    }
                    imageEditor->loadImage(path, qimg);
                } else {
                    imageEditor->clear();
                }
            } else if (currentExt == "tex" || currentExt == "spr" || currentExt == "pic") {
                TEXFile tex = TEX_Parse(path.toStdString());
                if (tex.valid && !tex.images.empty()) {
                    const auto& img = tex.images[0];
                    QImage qimg(img.width, img.height, QImage::Format_ARGB32);
                    if (img.mode == 3 || img.mode == 67) {
                        for (uint32_t y = 0; y < img.height; ++y) {
                            for (uint32_t x = 0; x < img.width; ++x) {
                                size_t i = (y * img.width + x) * 4;
                                qimg.setPixelColor(x, y, QColor(img.pixels[i+2], img.pixels[i+1], img.pixels[i], img.pixels[i+3]));
                            }
                        }
                    } else if (img.mode == 2) {
                        for (uint32_t y = 0; y < img.height; ++y) {
                            for (uint32_t x = 0; x < img.width; ++x) {
                                size_t i = (y * img.width + x) * 2;
                                uint16_t c = (img.pixels[i+1] << 8) | img.pixels[i];
                                // ARGB1555: bit15=A, bits14-10=R, bits9-5=G, bits4-0=B
                                int r = (c >> 10) & 0x1F; r = (r << 3) | (r >> 2);
                                int g = (c >>  5) & 0x1F; g = (g << 3) | (g >> 2);
                                int b =  c        & 0x1F; b = (b << 3) | (b >> 2);
                                qimg.setPixelColor(x, y, QColor(r, g, b, 255));
                            }
                        }
                    }
                    imageEditor->loadImage(path, qimg);
                } else {
                    imageEditor->clear();
                }
            } else {
                QImage img = loadImageSafe(path);
                if (!img.isNull()) {
                    imageEditor->loadImage(path, img);
                } else {
                    imageEditor->clear();
                }
            }
            imageEditor->show();
            if (imageEditor->toolsWidget) imageEditor->toolsWidget->show();
        } else if (mode == 4) { // Audio
            // Audio mode is a "transport-driven" mode for .wav files.
            // We convert the ILSF source to a cached PCM .wav and feed
            // it to MCI.  The audio bar (Play / Pause / Resume / Back /
            // Forward / Stop / scrubber / volume) is shown above the
            // viewer.  If the file is already a standard WAV we skip
            // the conversion step entirely (mciOpen handles it).
            QFileInfo fi(path);
            if (fi.exists()) {
                // Audio mode only makes sense for .wav files.
                if (fi.suffix().toLower() != "wav") {
                    logMessage("[WARN] Audio mode: " + fi.fileName() + " is not a .wav file");
                    if (audioFileLabel) audioFileLabel->setText(fi.fileName() + " (not audio)");
                } else {
                    audioSourceName = fi.fileName();
                    // Quick sniff: a standard WAV starts with "RIFF".
                    bool alreadyPcm = false;
                    QFile sniff(path);
                    if (sniff.open(QIODevice::ReadOnly)) {
                        char sig[4]; sniff.read(sig, 4);
                        if (std::memcmp(sig, "RIFF", 4) == 0) alreadyPcm = true;
                        sniff.close();
                    }
                    if (alreadyPcm) {
                        mciOpen(path);
                    } else {
                        audioLoadIgiWav(path, /*autoPlay=*/false);
                    }
                }
            }
            if (audioBar) audioBar->show();
        } else if (mode == 5) { // Animation (unified .iff / .mef)
            // The Animation mode is a single viewer for skeletal
            // animations.  .iff files show bones only (dots/lines);
            // .mef files show the textured 3D model.  Both get the
            // video-player transport controls and the animation panel.
            modelViewer->loadModel(path);
            modelViewer->show();
            if (iffMediaBar) {
                iffMediaBar->show();
                if (iffBtnPlay) iffBtnPlay->setText(modelViewer->iffPlaying ? "\u23f8" : "\u25b6");
            }
            if (animationPanel) animationPanel->show();
        } else if (mode == 6) { // 3D
            modelViewer->loadModel(path);
            modelViewer->show();
        }
    }

    void executeCommand(const QString& cmd) {
        if (currentFile.isEmpty()) return;

        QFileInfo info(currentFile);
        QString outDir = info.absolutePath();
        QString baseName = info.completeBaseName();

        QStringList args = cmd.split(" ", Qt::SkipEmptyParts);

        if (cmd == "tex decode-batch") {
            args.clear();
            args << "tex" << "decode" << info.absoluteDir().absolutePath() << "-o" << outDir << "--batch";
        } else if (cmd == "mef export-bundle") {
            QString target = currentFile;
            QString outBundle = QFileDialog::getExistingDirectory(this, "Select Export Folder", outDir);
            if (outBundle.isEmpty()) return;
            QString datFile = globalLevelDatPath;
            if (datFile.isEmpty()) datFile = QFileDialog::getOpenFileName(this, "Select DAT File", outDir, "DAT Files (*.dat)");
            if (datFile.isEmpty()) return;
            QString texDir = globalTextureDir;
            if (texDir.isEmpty()) texDir = QFileDialog::getExistingDirectory(this, "Select Textures Directory", outDir);
            if (texDir.isEmpty()) return;
            logMessage(QString("[INFO] Exporting Bundle to %1 using DAT: %2").arg(outBundle, datFile));
            args.clear();
            args << "mef" << "bundle" << target << "-o" << outBundle << "--dat" << datFile << "--texdir" << texDir;
        } else if (cmd == "mef apply-tex") {
            QString target = currentFile;
            QString outBundle = globalCacheDir + "/bundle";
            QString datFile = globalLevelDatPath;
            if (datFile.isEmpty()) {
                datFile = QFileDialog::getOpenFileName(this, "Select DAT File for Bundle", outDir, "DAT Files (*.dat)");
            }
            if (datFile.isEmpty()) return;
            QString texDir = globalTextureDir;
            if (texDir.isEmpty()) {
                texDir = QFileDialog::getExistingDirectory(this, "Select Textures Directory for Bundle", outDir);
            }
            if (texDir.isEmpty()) return;
            logMessage(QString("[INFO] Applying Textures using DAT: %1, TexDir: %2").arg(datFile, texDir));
            args.clear();
            args << "mef" << "bundle" << target << "-o" << outBundle << "--dat" << datFile << "--texdir" << texDir << "--no-obj";
        } else if (cmd == "mef apply-tex-all") {
            // Bundle ALL .mef files in the folder one by one
            QString folderPath = currentFile; // must be a directory
            if (!QFileInfo(folderPath).isDir()) folderPath = QFileInfo(folderPath).absolutePath();
            
            QString datFile = globalLevelDatPath;
            if (datFile.isEmpty()) {
                datFile = QFileDialog::getOpenFileName(this, "Select DAT File for Bundle", folderPath, "DAT Files (*.dat)");
            }
            if (datFile.isEmpty()) return;
            QString texDir = globalTextureDir;
            if (texDir.isEmpty()) {
                texDir = QFileDialog::getExistingDirectory(this, "Select Textures Directory for Bundle", folderPath);
            }
            if (texDir.isEmpty()) return;
            
            QString outBundle = globalCacheDir + "/bundle";
            QDir bundleDir(outBundle);
            bundleDir.mkpath(".");
            
            QDir dir(folderPath);
            QStringList mefs = dir.entryList(QStringList() << "*.mef" << "*.MEF", QDir::Files);
            logMessage(QString("[INFO] Apply Textures to All: found %1 MEF files in %2").arg(mefs.size()).arg(folderPath));
            
            QProgressDialog progress("Extracting textures for all MEFs in folder...", "Cancel", 0, mefs.size(), this);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(0);
            progress.show();
            
            QString firstModel;
            int bundled = 0;
            for (int i = 0; i < mefs.size(); ++i) {
                if (progress.wasCanceled()) {
                    logMessage("[WARN] Apply Textures All was canceled by user.");
                    break;
                }
                const QString& mefName = mefs[i];
                progress.setValue(i);
                progress.setLabelText(QString("Extracting %1 (%2 of %3)...").arg(mefName).arg(i+1).arg(mefs.size()));
                qApp->processEvents();

                QString mefPath = folderPath + "/" + mefName;
                QString mefBase = QFileInfo(mefName).completeBaseName();
                QStringList bargs;
                bargs << "mef" << "bundle" << mefPath << "-o" << outBundle << "--dat" << datFile << "--texdir" << texDir << "--no-obj";
                logMessage(QString("> igi1conv %1").arg(bargs.join(" ")));
                QProcess proc;
                proc.setProgram(qApp->applicationFilePath());
                proc.setArguments(bargs);
                proc.start();
                proc.waitForFinished(30000);
                QString out = proc.readAllStandardOutput();
                QString err = proc.readAllStandardError();
                if (!out.isEmpty()) logMessage(out.trimmed());
                if (!err.isEmpty()) logMessage("ERROR: " + err.trimmed());
                
                if (firstModel.isEmpty()) {
                    firstModel = folderPath + "/" + mefName; // Load the original MEF file!
                }
                bundled++;
            }
            progress.setValue(mefs.size());
            logMessage(QString("[INFO] Apply Textures All complete: %1/%2 models processed").arg(bundled).arg(mefs.size()));
            if (!firstModel.isEmpty()) {
                modelViewer->loadModel(firstModel);
                viewModeCombo->blockSignals(true);
                viewModeCombo->setCurrentIndex(6);
                viewModeCombo->blockSignals(false);
                hideAllViewers();
                modelViewer->show();
                logMessage("[INFO] Loaded first bundled MEF in 3D View: " + firstModel);
            }
            return; // Already ran all sub-processes above

        } else if (cmd == "mef export-bundle-all") {
            QString folderPath = currentFile;
            if (!QFileInfo(folderPath).isDir()) folderPath = QFileInfo(folderPath).absolutePath();
            
            QString outBundle = QFileDialog::getExistingDirectory(this, "Select Export Folder", folderPath);
            if (outBundle.isEmpty()) return;
            
            QString datFile = globalLevelDatPath;
            if (datFile.isEmpty()) datFile = QFileDialog::getOpenFileName(this, "Select DAT File", folderPath, "DAT Files (*.dat)");
            if (datFile.isEmpty()) return;
            QString texDir = globalTextureDir;
            if (texDir.isEmpty()) texDir = QFileDialog::getExistingDirectory(this, "Select Textures Directory", folderPath);
            if (texDir.isEmpty()) return;
            
            QDir dir(folderPath);
            QStringList mefs = dir.entryList(QStringList() << "*.mef" << "*.MEF", QDir::Files);
            logMessage(QString("[INFO] Export All: found %1 MEF files in %2").arg(mefs.size()).arg(folderPath));
            
            QProgressDialog progress("Exporting all MEFs in folder...", "Cancel", 0, mefs.size(), this);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(0);
            progress.show();
            
            int bundled = 0;
            for (int i = 0; i < mefs.size(); ++i) {
                if (progress.wasCanceled()) {
                    logMessage("[WARN] Export All was canceled by user.");
                    break;
                }
                const QString& mefName = mefs[i];
                progress.setValue(i);
                progress.setLabelText(QString("Exporting %1 (%2 of %3)...").arg(mefName).arg(i+1).arg(mefs.size()));
                qApp->processEvents();

                QString mefPath = folderPath + "/" + mefName;
                QStringList bargs;
                bargs << "mef" << "bundle" << mefPath << "-o" << outBundle << "--dat" << datFile << "--texdir" << texDir;
                logMessage(QString("> igi1conv %1").arg(bargs.join(" ")));
                QProcess proc;
                proc.setProgram(qApp->applicationFilePath());
                proc.setArguments(bargs);
                proc.start();
                proc.waitForFinished(30000);
                QString out = proc.readAllStandardOutput();
                QString err = proc.readAllStandardError();
                if (!out.isEmpty()) logMessage(out.trimmed());
                if (!err.isEmpty()) logMessage("ERROR: " + err.trimmed());
                bundled++;
            }
            progress.setValue(mefs.size());
            logMessage(QString("[INFO] Export-All complete: %1/%2 models bundled").arg(bundled).arg(mefs.size()));
            return; // Already ran all sub-processes above

        } else if (cmd == "mef to-text" || cmd == "mex to-text") {
            QString outTxt = currentFile;
            args.clear();
            if (cmd == "mex to-text") {
                args << "mef" << "dump" << currentFile << "-o" << outTxt;
                logMessage(QString("[INFO] Dumping MEX text %1 to: %2").arg(currentExt.toUpper(), outTxt));
            } else {
                args << "mef" << "to-text" << currentFile << "-o" << outTxt;
                logMessage(QString("[INFO] Exporting text %1 to: %2").arg(currentExt.toUpper(), outTxt));
            }
        } else if (cmd == "mef compile-text" || cmd == "mex compile-text") {
            QString outBin = currentFile;
            args.clear();
            args << "mef" << "compile" << currentFile << "-o" << outBin;
            logMessage(QString("[INFO] Compiling text %1 to binary: %2").arg(currentExt.toUpper(), outBin));
        } else if (cmd == "res unpack") {
            args << currentFile << (outDir + "/" + baseName + "_unpacked");
        } else if (cmd == "mef export" || cmd == "mex export") {
            QString outFolder = QFileDialog::getExistingDirectory(this, "Select Export Folder", outDir);
            if (outFolder.isEmpty()) return;
            args << currentFile << "-o" << (outFolder + "/" + baseName + ".obj");
        } else if (cmd == "mef build-rigid") {
            QString outFolder = QFileDialog::getExistingDirectory(this, "Select Output Folder for Rigid Model", outDir);
            if (outFolder.isEmpty()) return;
            args << currentFile << "-o" << (outFolder + "/" + baseName + ".mef");
            if (!globalLevelDatPath.isEmpty()) args << "--dat" << globalLevelDatPath;
            if (!globalTextureDir.isEmpty())   args << "--texdir" << globalTextureDir;
        } else {
            args << currentFile;
            if (cmd == "qsc compile") args << "-o" << (outDir + "/" + baseName + ".qvm");
            else if (cmd == "qvm decompile") args << "-o" << (outDir + "/" + baseName + ".qsc");
            else if (cmd == "mef dump" || cmd == "mex dump") args << "-o" << (outDir + "/" + baseName + "_dump.txt");
            else if (cmd == "res extract") args << "-o" << outDir;
            else if (cmd == "fnt export") args << "-o" << (outDir + "/" + baseName + ".png");
            else if (cmd == "qvm disasm") args << "-o" << (outDir + "/" + baseName + ".qas");
        }

        if (args.isEmpty()) return;

        QString progressText = "Executing command...";
        if (cmd.endsWith("build-rigid")) {
            progressText = "Completing Build model finding attachments textures.... Please wait";
        }
        QProgressDialog progress(progressText, "Cancel", 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.show();
        qApp->processEvents();

        QProcess process;
        process.setProgram(qApp->applicationFilePath());
        process.setArguments(args);
        
        logMessage(QString("> igi1conv %1").arg(args.join(" ")));
        process.start();
        
        while (!process.waitForFinished(100)) {
            qApp->processEvents();
            if (progress.wasCanceled()) {
                logMessage("[WARN] Command was canceled by user.");
                process.kill();
                process.waitForFinished();
                break;
            }
        }

        QString output = process.readAllStandardOutput();
        QString err = process.readAllStandardError();
        
        if (!output.isEmpty()) logMessage(output.trimmed());
        if (!err.isEmpty()) logMessage("ERROR: " + err.trimmed());
        logMessage("--------------------------------------------------");
    }
};

int run_gui() {
    int argc = 1;
    char arg0[] = "igi1conv.exe";
    char* argv[] = { arg0, nullptr };

    QApplication app(argc, argv);
    app.setStyle("Fusion");
    {
        QIcon ico(":/igi1conv.ico");
        if (!ico.isNull()) app.setWindowIcon(ico);
    }
    QString iniPath = QCoreApplication::applicationDirPath() + "/igi1conv.ini";
    QString theme = QSettings(iniPath, QSettings::IniFormat).value("Theme", "Dark").toString();
    
    auto applyTheme = [&](const QString& t) {
        if (t == "Light") {
            QPalette p;
            p.setColor(QPalette::Window, QColor(240,240,240));
            p.setColor(QPalette::WindowText, Qt::black);
            p.setColor(QPalette::Base, Qt::white);
            p.setColor(QPalette::AlternateBase, QColor(233,233,233));
            p.setColor(QPalette::ToolTipBase, Qt::white);
            p.setColor(QPalette::ToolTipText, Qt::black);
            p.setColor(QPalette::Text, Qt::black);
            p.setColor(QPalette::Button, QColor(240,240,240));
            p.setColor(QPalette::ButtonText, Qt::black);
            p.setColor(QPalette::BrightText, Qt::red);
            p.setColor(QPalette::Link, QColor(0,0,200));
            p.setColor(QPalette::Highlight, QColor(0,120,215));
            p.setColor(QPalette::HighlightedText, Qt::white);
            app.setPalette(p);
            app.setStyleSheet(
                "QTextEdit{background:white;color:black;border:1px solid #ccc;}"
                "QPlainTextEdit{background:white;color:black;border:1px solid #ccc;}"
                "QTreeView{background:white;color:black;alternate-background-color:#f0f0f0;}"
                "QLineEdit{background:white;color:black;border:1px solid #aaa;border-radius:3px;}"
                "QToolBar{background:#ebebeb;border-bottom:1px solid #ccc;spacing:4px;padding:2px 6px;}"
                "QMenuBar{background:#f0f0f0;color:black;}"
                "QMenu{background:#f0f0f0;color:black;}"
                "QStatusBar{background:#f0f0f0;color:black;}"
                "QLabel{color:black;}"
                "QSplitter::handle{background:#ccc;}"
            );
        } else if (t == "Solarized") {
            QPalette p;
            p.setColor(QPalette::Window, QColor(0,43,54));
            p.setColor(QPalette::WindowText, QColor(131,148,150));
            p.setColor(QPalette::Base, QColor(7,54,66));
            p.setColor(QPalette::AlternateBase, QColor(0,43,54));
            p.setColor(QPalette::Text, QColor(131,148,150));
            p.setColor(QPalette::Button, QColor(0,43,54));
            p.setColor(QPalette::ButtonText, QColor(131,148,150));
            p.setColor(QPalette::Highlight, QColor(38,139,210));
            p.setColor(QPalette::HighlightedText, Qt::white);
            app.setPalette(p);
            app.setStyleSheet("");
        } else if (t == "Military") {
            QPalette p;
            p.setColor(QPalette::Window, QColor(30,40,20));
            p.setColor(QPalette::WindowText, QColor(120,200,60));
            p.setColor(QPalette::Base, QColor(15,25,10));
            p.setColor(QPalette::AlternateBase, QColor(30,40,20));
            p.setColor(QPalette::Text, QColor(120,200,60));
            p.setColor(QPalette::Button, QColor(30,40,20));
            p.setColor(QPalette::ButtonText, QColor(120,200,60));
            p.setColor(QPalette::Highlight, QColor(80,140,30));
            p.setColor(QPalette::HighlightedText, Qt::black);
            app.setPalette(p);
            app.setStyleSheet("");
        } else {
            // Dark (default)
            QPalette p;
            p.setColor(QPalette::Window, QColor(53,53,53));
            p.setColor(QPalette::WindowText, Qt::white);
            p.setColor(QPalette::Base, QColor(25,25,25));
            p.setColor(QPalette::AlternateBase, QColor(53,53,53));
            p.setColor(QPalette::ToolTipBase, Qt::white);
            p.setColor(QPalette::ToolTipText, Qt::white);
            p.setColor(QPalette::Text, Qt::white);
            p.setColor(QPalette::Button, QColor(53,53,53));
            p.setColor(QPalette::ButtonText, Qt::white);
            p.setColor(QPalette::BrightText, Qt::red);
            p.setColor(QPalette::Link, QColor(42,130,218));
            p.setColor(QPalette::Highlight, QColor(42,130,218));
            p.setColor(QPalette::HighlightedText, Qt::black);
            app.setPalette(p);
            app.setStyleSheet("");
        }
    };
    applyTheme(theme);

    MainWindow window;
    window.show();

    return app.exec();
}

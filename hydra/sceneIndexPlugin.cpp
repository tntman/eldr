// HdVk's scene index plugin, appended after UsdImaging (so after
// UsdSkelImaging) for this renderer:
// - turns implicit prims (UsdGeomCube, Sphere, Cone, Cylinder, Capsule,
//   Plane) into meshes, same as hdEmbree does;
// - lets the render delegate re-sync every mesh when a render setting
//   that shapes them (subdivision, motion blur) changes (HdVkDirtyPrims);
// - resolves velocities, accelerations and angularVelocities into sampled
//   points and instance positions and rotations (HdsiVelocityMotion-
//   ResolvingSceneIndex) where Velocity Blur asks for it
//   (_VelocityModeSceneIndex);
// - skips UsdSkelImaging's CPU skinning: for a skinned mesh it hands the
//   ExtComputation inputs over as primvars (the same hydra:* primvars
//   UsdSkelImaging writes itself with HD_ENABLE_DEFERRED_SKINNING), so
//   HdVkMesh skins on the GPU and per frame only the skinning transforms
//   change. Authored normals come back the same way (UsdSkelImaging blocks
//   them on skinned meshes) and are skinned on the GPU too. With deferred
//   skinning (HD_ENABLE_DEFERRED_SKINNING=1) those primvars come as they are, and
//   instanced skeletons' per-instance transforms are remapped to the mesh's
//   own skel:joints here (UsdSkelImaging remaps only the first instance's).
#include "renderDelegate.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/extComputationPrimvarsSchema.h"
#include "pxr/imaging/hd/extComputationSchema.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/legacyDisplayStyleSchema.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/sceneIndexPrimView.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h"
#include "pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/work/withScopedParallelism.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// UsdSkelImaging's computation and input names (usdSkelImaging/tokens.h),
// spelled out so HdVk doesn't link UsdImaging.
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((aggregator, "skinningPointsInputAggregatorComputation"))
    ((computation, "skinningPointsComputation"))
    ((normalsAggregator, "skinningNormalsInputAggregatorComputation"))
    (restPoints)
    (restNormals)
    (hasFaceVaryingNormals)
    (geomBindXform)
    (influences)
    (numInfluencesPerComponent)
    (hasConstantInfluences)
    (skinningXforms)
    (skinningDualQuats)
    (skelLocalToWorld)
    (primWorldToLocal)
    (blendShapeOffsets)
    (blendShapeOffsetRanges)
    (numBlendShapeOffsetRanges)
    (blendShapeWeights)
    (skelBinding)
    (skeleton)
    (joints)
    (resolvedSkeleton)
    (skinningTransforms)
    (skelLocalToCommonSpace)
    ((eldrVblur, "eldr:object:vblur"))
    ((karmaVblur, "karma:object:vblur"))
    ((eldrInstanceVblur, "eldr:object:instance_vblur"))
    ((karmaInstanceVblur, "karma:object:instance_vblur")));

// A skeleton's resolved data (resolvedSkeleton/<name>) at a shutter
// offset, computed once for all the meshes it skins: UsdSkelImaging builds
// a new data source per request, which poses every instance at offset 0
// when made and again at each other offset asked, so a crowd's meshes
// posed their skeleton up to four times each per frame. Kept until the
// scene's next notice (a new time dirties the skeleton), so it is exact.
// HDVK_NO_POSE_SHARE=1 (A/B): each mesh poses its own, as before.
// ponytail: the last frame's poses stay in RAM until the next notice
// (crowd_9k: ~50 MB); clear at the end of a sync if that ever matters.
class _SkelPoses
{
public:
    VtValue Value(SdfPath const &skeleton, TfToken const &name, HdContainerDataSourceHandle const &resolved,
                  HdSampledDataSource::Time t)
    {
        const std::shared_ptr<_Entry> e = _Find(skeleton, name, resolved);
        if (!e->source) {
            return VtValue();
        }
        std::shared_ptr<_Value> v;
        {
            std::lock_guard<std::mutex> lock(e->mutex);
            std::shared_ptr<_Value> &slot = e->values[t];
            if (!slot) {
                slot = std::make_shared<_Value>();
            }
            v = slot;
        }
        _Once(v->once, [&] { v->value = e->source->GetValue(t); });
        return v->value;
    }

    HdSampledDataSourceHandle Source(SdfPath const &skeleton, TfToken const &name,
                                     HdContainerDataSourceHandle const &resolved)
    {
        return _Find(skeleton, name, resolved)->source;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.clear();
    }

private:
    struct _Value
    {
        std::once_flag once;
        VtValue value;
    };
    struct _Entry
    {
        std::once_flag once;
        HdSampledDataSourceHandle source;
        std::mutex mutex;
        std::map<HdSampledDataSource::Time, std::shared_ptr<_Value>> values;
    };

    // Another mesh of the skeleton waits for the first one's pose. Isolated,
    // so the thread posing (UsdSkelImaging poses instances in parallel)
    // doesn't pick up another mesh's sync that would wait on itself.
    template <typename Fn>
    static void _Once(std::once_flag &once, Fn &&fn)
    {
        std::call_once(once, [&] { WorkWithScopedParallelism(fn, false); });
    }

    std::shared_ptr<_Entry> _Find(SdfPath const &skeleton, TfToken const &name,
                                  HdContainerDataSourceHandle const &resolved)
    {
        std::shared_ptr<_Entry> e;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::shared_ptr<_Entry> &slot = _entries[{skeleton, name}];
            if (!slot) {
                slot = std::make_shared<_Entry>();
            }
            e = slot;
        }
        _Once(e->once, [&] { e->source = HdSampledDataSource::Cast(resolved->Get(name)); });
        return e;
    }

    std::mutex _mutex;
    std::map<std::pair<SdfPath, TfToken>, std::shared_ptr<_Entry>> _entries;
};

// A skeleton's resolved value as one of its meshes' primvars, from the
// shared poses.
template <typename T>
class _SharedSkelDataSource final : public HdTypedSampledDataSource<T>
{
public:
    HD_DECLARE_DATASOURCE(_SharedSkelDataSource<T>);
    using Time = HdSampledDataSource::Time;

    VtValue GetValue(Time t) override { return _poses->Value(_skeleton, _name, _resolved, t); }

    T GetTypedValue(Time t) override { return GetValue(t).template GetWithDefault<T>(T()); }

    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override
    {
        const HdSampledDataSourceHandle source = _poses->Source(_skeleton, _name, _resolved);
        return source && source->GetContributingSampleTimesForInterval(start, end, times);
    }

private:
    _SharedSkelDataSource(std::shared_ptr<_SkelPoses> poses, SdfPath skeleton, TfToken name,
                          HdContainerDataSourceHandle resolved)
        : _poses(std::move(poses)), _skeleton(std::move(skeleton)), _name(std::move(name)),
          _resolved(std::move(resolved))
    {
    }

    const std::shared_ptr<_SkelPoses> _poses;
    const SdfPath _skeleton;
    const TfToken _name;
    const HdContainerDataSourceHandle _resolved;
};

// The skinning transforms of every instance of a skeleton (skeleton joint
// order, instance after instance) in a mesh's skel:joints order: what
// UsdSkelAnimMapper::RemapTransforms does for one instance, for each. A mesh
// joint the skeleton lacks gets the identity, as there.
class _InstanceJointsDataSource final : public HdTypedSampledDataSource<VtMatrix4fArray>
{
public:
    HD_DECLARE_DATASOURCE(_InstanceJointsDataSource);

    VtValue GetValue(Time t) override { return VtValue(GetTypedValue(t)); }

    VtMatrix4fArray GetTypedValue(Time t) override
    {
        const VtMatrix4fArray source = _skeleton->GetTypedValue(t);
        const size_t from = _skelJoints, to = _map.size(), instances = from ? source.size() / from : 0;
        VtMatrix4fArray result(instances * to, GfMatrix4f(1.0f));
        GfMatrix4f *out = result.data();
        for (size_t i = 0; i < instances; ++i) {
            for (size_t j = 0; j < to; ++j, ++out) {
                if (_map[j] >= 0) {
                    *out = source[i * from + size_t(_map[j])];
                }
            }
        }
        return result;
    }

    bool GetContributingSampleTimesForInterval(Time start, Time end, std::vector<Time> *times) override
    {
        return _skeleton->GetContributingSampleTimesForInterval(start, end, times);
    }

private:
    _InstanceJointsDataSource(HdTypedSampledDataSource<VtMatrix4fArray>::Handle skeleton, size_t skelJoints,
                              std::vector<int> map)
        : _skeleton(std::move(skeleton)), _skelJoints(skelJoints), _map(std::move(map))
    {
    }

    const HdTypedSampledDataSource<VtMatrix4fArray>::Handle _skeleton;
    const size_t _skelJoints;
    const std::vector<int> _map;  // skeleton joint of each mesh joint, -1: none
};

TF_DECLARE_REF_PTRS(_SkinningSceneIndex);

class _SkinningSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _SkinningSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input)
    {
        return TfCreateRefPtr(new _SkinningSceneIndex(input));
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
        if (prim.primType != HdPrimTypeTokens->mesh || !prim.dataSource) {
            return prim;
        }
        const HdContainerDataSourceHandle aggregator =
            _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->aggregator)).dataSource;
        const HdContainerDataSourceHandle computation =
            _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->computation)).dataSource;
        if (!aggregator || !computation) {
            return _RemapInstanceJoints(prim);
        }
        const HdSampledDataSourceContainerSchema rest =
            HdExtComputationSchema::GetFromParent(aggregator).GetInputValues();
        const HdSampledDataSourceContainerSchema pose =
            HdExtComputationSchema::GetFromParent(computation).GetInputValues();

        auto primvar = [](HdSampledDataSourceHandle const &value, TfToken const &interpolation,
                          TfToken const &role) -> HdDataSourceBaseHandle {
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(value)
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
                .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
                .Build();
        };
        const TfToken &constant = HdPrimvarSchemaTokens->constant;
        TfTokenVector names = {
            HdTokens->points,
            HdSkinningSkelInputTokens->geomBindTransform,
            HdSkinningInputTokens->influences,
            HdSkinningInputTokens->numInfluencesPerComponent,
            HdSkinningInputTokens->hasConstantInfluences,
            HdSkinningInputTokens->skinningXforms,
            HdSkinningInputTokens->skelLocalToCommonSpace,
            HdSkinningInputTokens->commonSpaceToPrimLocal,
            HdSkinningInputTokens->blendShapeOffsets,
            HdSkinningInputTokens->blendShapeOffsetRanges,
            HdSkinningInputTokens->numBlendShapeOffsetRanges,
            HdSkinningInputTokens->blendShapeWeights,
        };
        std::vector<HdDataSourceBaseHandle> values = {
            primvar(rest.Get(_tokens->restPoints), HdPrimvarSchemaTokens->vertex, HdPrimvarRoleTokens->point),
            primvar(rest.Get(_tokens->geomBindXform), constant, TfToken()),
            primvar(rest.Get(_tokens->influences), constant, TfToken()),
            primvar(rest.Get(_tokens->numInfluencesPerComponent), constant, TfToken()),
            primvar(rest.Get(_tokens->hasConstantInfluences), constant, TfToken()),
            primvar(pose.Get(_tokens->skinningXforms), constant, TfToken()),
            primvar(pose.Get(_tokens->skelLocalToWorld), constant, TfToken()),
            primvar(pose.Get(_tokens->primWorldToLocal), constant, TfToken()),
            primvar(rest.Get(_tokens->blendShapeOffsets), constant, TfToken()),
            primvar(rest.Get(_tokens->blendShapeOffsetRanges), constant, TfToken()),
            primvar(rest.Get(_tokens->numBlendShapeOffsetRanges), constant, TfToken()),
            primvar(pose.Get(_tokens->blendShapeWeights), constant, TfToken()),
        };
        // Authored normals are blocked too; they reach only the normals
        // aggregator, which UsdSkelImaging serves for every skinned mesh
        // (announced as a prim only with USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS,
        // without which Storm shades skinned meshes faceted).
        const HdSampledDataSourceContainerSchema normalsInputs =
            HdExtComputationSchema::GetFromParent(
                _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->normalsAggregator)).dataSource)
                .GetInputValues();
        if (const HdSampledDataSourceHandle normals = normalsInputs.Get(_tokens->restNormals)) {
            const HdSampledDataSourceHandle faceVarying = normalsInputs.Get(_tokens->hasFaceVaryingNormals);
            names.push_back(HdTokens->normals);
            values.push_back(primvar(normals,
                                     faceVarying && faceVarying->GetValue(0.0f).GetWithDefault<bool>(false)
                                         ? HdPrimvarSchemaTokens->faceVarying
                                         : HdPrimvarSchemaTokens->vertex,
                                     HdPrimvarRoleTokens->normal));
        }
        // The skinning method, as UsdSkelImaging's deferred skinning
        // announces it: its pose inputs have dual quaternions only for
        // "dualQuaternion" (HdVk takes them from the joint matrices).
        names.push_back(HdSkinningInputTokens->numSkinningMethod);
        values.push_back(primvar(HdRetainedTypedSampledDataSource<int>::New(
                                     pose.Get(_tokens->skinningDualQuats) ? 1 : 0),
                                 constant, TfToken()));
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                HdPrimvarsSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()),
                HdExtComputationPrimvarsSchema::GetSchemaToken(),
                HdBlockDataSource::New()),
            prim.dataSource);
        return prim;
    }

    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    _SkinningSceneIndex(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input)
    {
    }

    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override
    {
        // A mesh that gains its computations becomes skinned.
        _poses->Clear();
        HdSceneIndexObserver::DirtiedPrimEntries dirtied;
        for (const auto &e : entries) {
            if (_IsComputation(e.primPath)) {
                dirtied.emplace_back(e.primPath.GetParentPath(),
                                     HdDataSourceLocatorSet{HdPrimvarsSchema::GetDefaultLocator(),
                                                            HdExtComputationPrimvarsSchema::GetDefaultLocator()});
            }
        }
        _SendPrimsAdded(entries);
        if (!dirtied.empty()) {
            _SendPrimsDirtied(dirtied);
        }
    }

    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &entries) override
    {
        _poses->Clear();
        _SendPrimsRemoved(entries);
    }

    // Edits of the computations' inputs become edits of the mesh's primvars:
    // per frame the pose computation changes, which only dirties the
    // skinning transforms (DirtyPrimvar), not the points.
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override
    {
        _poses->Clear();
        HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
        for (const auto &e : entries) {
            if (!_IsComputation(e.primPath)) {
                continue;
            }
            auto primvar = [](TfToken const &name) { return HdPrimvarsSchema::GetDefaultLocator().Append(name); };
            HdDataSourceLocatorSet locators;
            if (e.primPath.GetNameToken() == _tokens->normalsAggregator) {
                locators = {primvar(HdTokens->normals)};
            } else if (e.primPath.GetNameToken() == _tokens->aggregator) {
                locators = {primvar(HdTokens->points), primvar(HdSkinningSkelInputTokens->geomBindTransform),
                            primvar(HdSkinningInputTokens->influences),
                            primvar(HdSkinningInputTokens->numInfluencesPerComponent),
                            primvar(HdSkinningInputTokens->hasConstantInfluences),
                            primvar(HdSkinningInputTokens->blendShapeOffsets),
                            primvar(HdSkinningInputTokens->blendShapeOffsetRanges),
                            primvar(HdSkinningInputTokens->numBlendShapeOffsetRanges)};
            } else {
                locators = {primvar(HdSkinningInputTokens->skinningXforms),
                            primvar(HdSkinningInputTokens->skelLocalToCommonSpace),
                            primvar(HdSkinningInputTokens->commonSpaceToPrimLocal),
                            primvar(HdSkinningInputTokens->blendShapeWeights)};
            }
            dirtied.emplace_back(e.primPath.GetParentPath(), locators);
        }
        _SendPrimsDirtied(dirtied);
    }

private:
    // Deferred skinning of an instanced skeleton (hydra:numJoints > 0: the
    // skinning transforms of all instances, numJoints each) whose mesh names
    // its own skel:joints: UsdSkelImaging remaps only the first instance's
    // (its XXX TODO in _SkinningXformsDataSource), the rest would be lost.
    // Remap every instance's from the resolved skeleton instead. The pose
    // (and the skeleton's place) comes from _SkelPoses, shared by the
    // skeleton's meshes; without a remap it is the skeleton's as it stands,
    // as UsdSkelImaging hands it over then.
    HdSceneIndexPrim _RemapInstanceJoints(HdSceneIndexPrim prim) const
    {
        static const bool share = !TfGetenvBool("HDVK_NO_POSE_SHARE", false);
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
        const HdSampledDataSourceHandle numJoints =
            primvars.GetPrimvar(HdSkinningInputTokens->numJoints).GetPrimvarValue();
        if (!numJoints || numJoints->GetValue(0.0f).GetWithDefault<int>(0) <= 0) {
            return prim;
        }
        const HdContainerDataSourceHandle binding = HdContainerDataSource::Cast(prim.dataSource->Get(_tokens->skelBinding));
        const auto meshJointsDs =
            binding ? HdTypedSampledDataSource<VtTokenArray>::Cast(binding->Get(_tokens->joints)) : nullptr;
        const auto skeletonDs =
            binding ? HdTypedSampledDataSource<SdfPath>::Cast(binding->Get(_tokens->skeleton)) : nullptr;
        if ((!meshJointsDs && !share) || !skeletonDs) {
            return prim;
        }
        const SdfPath skeletonPath = skeletonDs->GetTypedValue(0.0f);
        const HdContainerDataSourceHandle skeleton = _GetInputSceneIndex()->GetPrim(skeletonPath).dataSource;
        const HdContainerDataSourceHandle skelSchema =
            skeleton ? HdContainerDataSource::Cast(skeleton->Get(_tokens->skeleton)) : nullptr;
        const HdContainerDataSourceHandle resolved =
            skeleton ? HdContainerDataSource::Cast(skeleton->Get(_tokens->resolvedSkeleton)) : nullptr;
        const auto skelJointsDs =
            skelSchema ? HdTypedSampledDataSource<VtTokenArray>::Cast(skelSchema->Get(_tokens->joints)) : nullptr;
        if (!resolved) {
            return prim;
        }
        // Made on demand when shared: UsdSkelImaging poses every instance
        // when it makes one.
        const HdTypedSampledDataSource<VtMatrix4fArray>::Handle transforms =
            share ? _SharedSkelDataSource<VtMatrix4fArray>::New(_poses, skeletonPath, _tokens->skinningTransforms,
                                                                resolved)
                  : HdTypedSampledDataSource<VtMatrix4fArray>::Cast(resolved->Get(_tokens->skinningTransforms));
        const VtTokenArray meshJoints = meshJointsDs ? meshJointsDs->GetTypedValue(0.0f) : VtTokenArray();
        const VtTokenArray skelJoints = skelJointsDs ? skelJointsDs->GetTypedValue(0.0f) : VtTokenArray();
        if (!transforms) {
            return prim;
        }
        // No mapping: UsdSkelImaging hands over the skeleton's own; so do we.
        const bool identity = meshJoints.empty() || meshJoints == skelJoints;
        if (!identity && skelJoints.empty()) {
            return prim;  // UsdSkelImaging's null mapping (the skeleton's own too), left as it is
        }
        auto value = [](HdDataSourceBaseHandle const &ds) {
            return HdRetainedContainerDataSource::New(HdPrimvarSchemaTokens->primvarValue, ds);
        };
        if (identity) {
            if (!share) {
                return prim;
            }
            const TfToken names[] = {HdSkinningInputTokens->skinningXforms,
                                     HdSkinningInputTokens->skelLocalToCommonSpace};
            const HdDataSourceBaseHandle values[] = {
                value(transforms), value(_SharedSkelDataSource<GfMatrix4d>::New(
                                       _poses, skeletonPath, _tokens->skelLocalToCommonSpace, resolved))};
            prim.dataSource = HdOverlayContainerDataSource::New(
                HdRetainedContainerDataSource::New(HdPrimvarsSchema::GetSchemaToken(),
                                                   HdRetainedContainerDataSource::New(2, names, values)),
                prim.dataSource);
            return prim;
        }
        std::vector<int> map(meshJoints.size(), -1);
        for (size_t j = 0; j < meshJoints.size(); ++j) {
            const auto it = std::find(skelJoints.begin(), skelJoints.end(), meshJoints[j]);
            map[j] = it == skelJoints.end() ? -1 : int(it - skelJoints.begin());
        }
        auto constant = [](HdDataSourceBaseHandle const &value) {
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(HdSampledDataSource::Cast(value))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
                .Build();
        };
        TfTokenVector names = {HdSkinningInputTokens->skinningXforms, HdSkinningInputTokens->numJoints};
        std::vector<HdDataSourceBaseHandle> values = {
            constant(_InstanceJointsDataSource::New(transforms, skelJoints.size(), std::move(map))),
            constant(HdRetainedTypedSampledDataSource<int>::New(int(meshJoints.size())))};
        if (share) {
            names.push_back(HdSkinningInputTokens->skelLocalToCommonSpace);
            values.push_back(value(_SharedSkelDataSource<GfMatrix4d>::New(
                _poses, skeletonPath, _tokens->skelLocalToCommonSpace, resolved)));
        }
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                HdPrimvarsSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(names.size(), names.data(), values.data())),
            prim.dataSource);
        return prim;
    }

    static bool _IsComputation(SdfPath const &path)
    {
        return path.IsPrimPath() && (path.GetNameToken() == _tokens->aggregator ||
                                     path.GetNameToken() == _tokens->computation ||
                                     path.GetNameToken() == _tokens->normalsAggregator);
    }

    const std::shared_ptr<_SkelPoses> _poses = std::make_shared<_SkelPoses>();
};

TF_DECLARE_REF_PTRS(_VelocityModeSceneIndex);

// Karma's Velocity Blur as HdsiVelocityMotionResolvingSceneIndex's
// __velocityMotionMode: primvars eldr:object:vblur, else karma:object:vblur
// (on a PointInstancer their instance_vblur twins). 0 or unauthored ignores
// velocities (Karma's default), 1 uses them without accelerations, 2 with.
class _VelocityModeSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _VelocityModeSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input)
    {
        return TfCreateRefPtr(new _VelocityModeSceneIndex(input));
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
        if (!prim.dataSource ||
            !HdsiVelocityMotionResolvingSceneIndex::PrimTypeSupportsVelocityMotion(prim.primType)) {
            return prim;
        }
        const TfTokenVector names = prim.primType == HdPrimTypeTokens->instancer
                                        ? TfTokenVector{_tokens->eldrInstanceVblur, _tokens->karmaInstanceVblur}
                                        : TfTokenVector{_tokens->eldrVblur, _tokens->karmaVblur};
        const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
        int mode = 0;
        for (const TfToken &name : names) {
            const HdSampledDataSourceHandle ds = primvars.GetPrimvar(name).GetPrimvarValue();
            VtValue v = ds ? ds->GetValue(0.0f) : VtValue();
            if (v.IsArrayValued()) {
                v = VtValue::Cast<VtIntArray>(v);
                v = v.IsHolding<VtIntArray>() && !v.UncheckedGet<VtIntArray>().empty()
                        ? VtValue(v.UncheckedGet<VtIntArray>()[0]) : VtValue();
            }
            v = VtValue::Cast<int>(v);
            if (v.IsHolding<int>()) {
                mode = v.UncheckedGet<int>();
                break;
            }
        }
        const auto &t = *HdsiVelocityMotionResolvingSceneIndexTokens;
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                t.velocityMotionMode,
                HdRetainedTypedSampledDataSource<TfToken>::New(mode >= 2   ? t.enable
                                                               : mode == 1 ? t.noAcceleration
                                                                           : t.ignore)),
            prim.dataSource);
        return prim;
    }

    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

private:
    explicit _VelocityModeSceneIndex(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input)
    {
    }

    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override
    {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &entries) override
    {
        _SendPrimsRemoved(entries);
    }
    // An edited vblur primvar is an edited mode.
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override
    {
        static const HdDataSourceLocator mode(HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode);
        static const HdDataSourceLocatorSet vblur = [] {
            HdDataSourceLocatorSet set;
            for (const TfToken &name : {_tokens->eldrVblur, _tokens->karmaVblur, _tokens->eldrInstanceVblur,
                                        _tokens->karmaInstanceVblur}) {
                set.insert(HdPrimvarsSchema::GetDefaultLocator().Append(name));
            }
            return set;
        }();
        HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
        for (auto &e : dirtied) {
            if (e.dirtyLocators.Intersects(vblur)) {
                e.dirtyLocators.insert(mode);
            }
        }
        _SendPrimsDirtied(dirtied);
    }
};

TF_DECLARE_REF_PTRS(_SettingsSceneIndex);

// Passes the scene through; Dirty dirties prims of some types so they sync
// again and re-read the render settings.
class _SettingsSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _SettingsSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input)
    {
        return TfCreateRefPtr(new _SettingsSceneIndex(input));
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override { return _GetInputSceneIndex()->GetPrim(path); }
    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

    void Dirty(TfTokenVector const &types, HdDataSourceLocatorSet const &locators)
    {
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        for (const SdfPath &path : HdSceneIndexPrimView(_GetInputSceneIndex())) {
            const TfToken type = _GetInputSceneIndex()->GetPrim(path).primType;
            if (std::find(types.begin(), types.end(), type) != types.end()) {
                entries.emplace_back(path, locators);
            }
        }
        _SendPrimsDirtied(entries);
    }

private:
    explicit _SettingsSceneIndex(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input)
    {
    }

    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override
    {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &entries) override
    {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override
    {
        _SendPrimsDirtied(entries);
    }
};

} // namespace

void HdVkDirtyPrims(HdSceneIndexBaseRefPtr const &terminal, TfTokenVector const &types,
                    HdDataSourceLocatorSet const &locators)
{
    // Find this renderer's _SettingsSceneIndex upstream of the terminal.
    std::vector<HdSceneIndexBaseRefPtr> todo = {terminal};
    std::set<HdSceneIndexBase *> seen;
    while (!todo.empty()) {
        HdSceneIndexBase *sceneIndex = get_pointer(todo.back());
        todo.pop_back();
        if (!sceneIndex || !seen.insert(sceneIndex).second) {
            continue;
        }
        if (auto *settings = dynamic_cast<_SettingsSceneIndex *>(sceneIndex)) {
            settings->Dirty(types, locators);
            return;
        }
        if (auto *filtering = dynamic_cast<HdFilteringSceneIndexBase *>(sceneIndex)) {
            for (const HdSceneIndexBaseRefPtr &input : filtering->GetInputScenes()) {
                todo.push_back(input);
            }
        }
    }
}

class HdVk_SceneIndexPlugin final : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        HdSceneIndexBaseRefPtr const &inputScene,
        HdContainerDataSourceHandle const &) override
    {
        HdDataSourceBaseHandle const toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(
                HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        // The active render settings prim (the scene globals') is marked
        // active, so the render products are written (renderProducts.h).
        // Velocities resolve into the points and instance samples that
        // sampled motion and deformation blur read (after the implicit
        // surfaces, as Storm's). Light linking: UsdLux collection:lightLink
        // and shadowLink as Hydra categories (lights' lightLink/shadowLink,
        // rprims' categories), as hdPrman.
        return HdsiLightLinkingSceneIndex::New(_SettingsSceneIndex::New(HdsiVelocityMotionResolvingSceneIndex::New(_VelocityModeSceneIndex::New(
            HdsiImplicitSurfaceSceneIndex::New(
                _SkinningSceneIndex::New(HdsiRenderSettingsFilteringSceneIndex::New(inputScene, nullptr)),
                HdRetainedContainerDataSource::New(
                    HdPrimTypeTokens->sphere, toMesh,
                    HdPrimTypeTokens->cube, toMesh,
                    HdPrimTypeTokens->cone, toMesh,
                    HdPrimTypeTokens->cylinder, toMesh,
                    HdPrimTypeTokens->capsule, toMesh,
                    HdPrimTypeTokens->plane, toMesh))))), nullptr);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdVk_SceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // Keyed by the renderer's displayName in plugInfo.json ("Eldr", HdVk's product name).
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Eldr", TfToken("HdVk_SceneIndexPlugin"),
        /*inputArgs=*/nullptr, /*insertionPhase=*/0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE

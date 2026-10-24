#include "volume.h"

#include "field.h"
#include "instancer.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, (openvdbAsset)(density)(temperature)(Cd)(color)(emission));

} // namespace

HdVkVolume::HdVkVolume(SdfPath const &id, vkr::Scene *scene)
    : HdVolume(id)
    , _scene(scene)
    , _volume(scene->AddVolume())
{
}

HdVkVolume::~HdVkVolume()
{
    _scene->RemoveVolume(_volume);
}

HdDirtyBits HdVkVolume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyVolumeField | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex;
}

void HdVkVolume::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    HdVolume::UpdateRenderTag(delegate, renderParam);
    _volume->tag = HdVkRenderTagBit(GetRenderTag());
}

void HdVkVolume::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits, TfToken const &)
{
    const SdfPath &id = GetId();
    vkr::Volume &v = *_volume;
    // Fields sync before rprims (bprims first), so their grids are there.
    if (*dirtyBits & HdChangeTracker::DirtyVolumeField) {
        v.density = v.temperature = v.color = v.emission = nullptr;
        std::shared_ptr<vkr::VolumeGrid> first;
        for (const HdVolumeFieldDescriptor &d : sceneDelegate->GetVolumeFieldDescriptors(id)) {
            if (d.fieldPrimType != _tokens->openvdbAsset) {
                continue;  // ponytail: OpenVDB assets only (no Field3D, no Houdini's live fields)
            }
            const auto *field = static_cast<const HdVkField *>(
                sceneDelegate->GetRenderIndex().GetBprim(d.fieldPrimType, d.fieldId));
            if (!field) {
                continue;
            }
            const std::shared_ptr<vkr::VolumeGrid> &grid = field->Grid();
            first = first ? first : grid;
            if (d.fieldName == _tokens->density) {
                v.density = grid;
            } else if (d.fieldName == _tokens->temperature) {
                v.temperature = grid;
            } else if (d.fieldName == _tokens->Cd || d.fieldName == _tokens->color) {
                v.color = grid;
            } else if (d.fieldName == _tokens->emission) {
                v.emission = grid;
            }
        }
        if (!v.density) {
            v.density = first;
        }
    }

    // As HdVkMesh: instancers are synced by the rprims that use them.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) || HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        v.transforms = HdVkWorldTransforms(sceneDelegate, id, GetInstancerId());
    }
    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        v.visible = IsVisible();
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE

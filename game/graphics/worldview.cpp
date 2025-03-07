#include "worldview.h"

#include <Tempest/Application>

#include "graphics/mesh/submesh/packedmesh.h"
#include "game/globaleffects.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "utils/gthfont.h"
#include "gothic.h"

using namespace Tempest;

WorldView::WorldView(const World& world, const PackedMesh& wmesh)
  : owner(world),sky(sGlobal,world,wmesh.bbox()),visuals(sGlobal,wmesh.bbox()),
    objGroup(visuals),pfxGroup(*this,sGlobal,visuals),land(visuals,wmesh) {
  pfxGroup.resetTicks();
  if(Gothic::inst().doRayQuery())
    tlasLand = Resources::device().tlas({{Matrix4x4::mkIdentity(),0,&land.rt.blas}});
  visuals.setLandscapeBlas(&land.rt.blas);
  visuals.onTlasChanged.bind(this,&WorldView::setupTlas);
  }

WorldView::~WorldView() {
  // cmd buffers must not be in use
  Resources::device().waitIdle();
  }

const Texture2d& WorldView::shadowLq() const {
  return sky.shadowLq();
  }

const LightSource& WorldView::mainLight() const {
  return sky.sunLight();
  }

const Tempest::Vec3& WorldView::ambientLight() const {
  return sky.ambientLight();
  }

bool WorldView::isInPfxRange(const Vec3& pos) const {
  return pfxGroup.isInPfxRange(pos);
  }

void WorldView::tick(uint64_t /*dt*/) {
  auto pl = owner.player();
  if(pl!=nullptr) {
    pfxGroup.setViewerPos(pl->position());
    }
  }

void WorldView::preFrameUpdate(const Matrix4x4& view, const Matrix4x4& proj,
                               float zNear, float zFar,
                               const Tempest::Matrix4x4* shadow,
                               uint64_t tickCount, uint8_t fId) {
  visuals.updateTlas(sGlobal.bindless,fId);

  updateLight();
  sGlobal.setViewProject(view,proj,zNear,zFar,shadow);

  pfxGroup.tick(tickCount);
  sGlobal.lights.tick(tickCount);
  sGlobal .setTime(tickCount);
  sGlobal .commitUbo(fId);

  sGlobal.lights.preFrameUpdate(fId);
  pfxGroup.preFrameUpdate(fId);
  visuals .preFrameUpdate(fId);
  }

void WorldView::setGbuffer(const Texture2d& emission, const Texture2d& diffuse,
                           const Texture2d& norm, const Texture2d& depth,
                           const Texture2d* sh[], const Texture2d& hiZ) {
  const Texture2d* shadow[Resources::ShadowLayers] = {};
  for(size_t i=0; i<Resources::ShadowLayers; ++i)
    if(sh[i]==nullptr || sh[i]->isEmpty())
      shadow[i] = &Resources::fallbackBlack(); else
      shadow[i] = sh[i];

  // wait before update all descriptors
  Resources::device().waitIdle();
  sGlobal.gbufEmission = &emission;
  sGlobal.gbufDiffuse  = &diffuse;
  sGlobal.gbufNormals  = &norm;
  sGlobal.gbufDepth    = &depth;
  sGlobal.hiZ          = &hiZ;
  sGlobal.skyLut       = &sky.skyLut();
  //sGlobal.tlas        = &tlas;
  sGlobal.setShadowMap(shadow);
  sGlobal.setResolution(uint32_t(diffuse.w()),uint32_t(diffuse.h()));
  setupUbo();
  }

void WorldView::dbgLights(DbgPainter& p) const {
  sGlobal.lights.dbgLights(p);
  }

void WorldView::prepareSky(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t frameId) {
  sky.prepareSky(cmd,frameId);
  }

void WorldView::visibilityPass(const Frustrum fr[]) {
  for(uint8_t i=0; i<SceneGlobals::V_Count; ++i)
    sGlobal.frustrum[i] = fr[i];
  visuals.visibilityPass(fr);
  }

void WorldView::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawHiZ(cmd,fId);
  }

void WorldView::drawShadow(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId, uint8_t layer) {
  visuals.drawShadow(cmd,fId,layer);
  }

void WorldView::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawGBuffer(cmd,fId);
  }

void WorldView::drawSky(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  sky.drawSky(cmd,fId);
  }

void WorldView::drawWater(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawWater(cmd,fId);
  }

void WorldView::drawTranslucent(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawTranslucent(cmd,fId);
  }

void WorldView::drawFog(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  sky.drawFog(cmd,fId);
  }

void WorldView::drawLights(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  sGlobal.lights.draw(cmd,fId);
  }

MeshObjects::Mesh WorldView::addView(std::string_view visual, int32_t headTex, int32_t teethTex, int32_t bodyColor) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,headTex,teethTex,bodyColor,false);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addView(const ProtoMesh* mesh) {
  if(mesh!=nullptr) {
    bool staticDraw = false;
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,staticDraw);
    }
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addItmView(std::string_view visual, int32_t material) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,material,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addAtachView(const ProtoMesh::Attach& visual, const int32_t version) {
  return MeshObjects::Mesh(objGroup,visual,version,false);
  }

MeshObjects::Mesh WorldView::addStaticView(const ProtoMesh* mesh, bool staticDraw) {
  if(mesh!=nullptr)
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,staticDraw);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addStaticView(std::string_view visual) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addDecalView(const phoenix::vob& vob) {
  if(auto mesh=Resources::decalMesh(vob))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

const AccelerationStructure& WorldView::landscapeTlas() {
  return tlasLand;
  }

void WorldView::updateLight() {
  const int64_t now = owner.time().timeInDay().toInt();
  sky.updateLight(now);

  sGlobal.setSunlight(sky.sunLight(), sky.ambientLight());
  }

void WorldView::setupUbo() {
  // cmd buffers must not be in use
  sGlobal.lights.setupUbo();
  sky.setupUbo();
  visuals.setupUbo();
  }

void WorldView::setupTlas(const Tempest::AccelerationStructure* tlas) {
  sGlobal.tlas = tlas;
  //sGlobal.tlas = &tlasLand;
  setupUbo();
  }

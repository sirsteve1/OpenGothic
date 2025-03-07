#include "objectsbucket.h"

#include <Tempest/Log>

#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "sceneglobals.h"

#include "utils/workers.h"
#include "visualobjects.h"
#include "shaders.h"
#include "gothic.h"

using namespace Tempest;

static uint32_t nextPot(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
  }

static Matrix4x4 dummyMatrix() {
  auto mat = Matrix4x4::mkIdentity();
  mat.set(0,0, 0.f);
  mat.set(1,1, 0.f);
  mat.set(2,2, 0.f);
  // so bbox test will fail
  mat.set(3,1, -1000000.f);
  return mat;
  }

void ObjectsBucket::Item::setObjMatrix(const Tempest::Matrix4x4 &mt) {
  owner->setObjMatrix(id,mt);
  }

void ObjectsBucket::Item::setAsGhost(bool g) {
  if(owner->mat.isGhost==g)
    return;

  auto m = owner->mat;
  m.isGhost = g;
  auto&  bucket = owner->owner.getBucket(owner->objType,m,owner->staticMesh,owner->animMesh,owner->instanceDesc);

  auto&  v      = owner->val[id];
  size_t idNext = size_t(-1);
  switch(owner->objType) {
    case Landscape:
    case LandscapeShadow: {
      idNext = bucket.alloc(*owner->staticMesh,v.iboOffset,v.iboLength,v.visibility.bounds(),owner->mat);
      break;
      }
    case Static:
    case Movable:
    case Morph: {
      idNext = bucket.alloc(*owner->staticMesh,v.iboOffset,v.iboLength,v.visibility.bounds(),owner->mat);
      break;
      }
    case Pfx: {
      idNext = bucket.alloc(v.vboM,v.visibility.bounds());
      break;
      }
    case Animated: {
      idNext = bucket.alloc(*owner->animMesh,v.iboOffset,v.iboLength,*v.skiningAni);
      break;
      }
    }
  if(idNext==size_t(-1))
    return;

  auto oldId = id;
  auto oldOw = owner;
  owner = &bucket;
  id    = idNext;

  auto& v2 = owner->val[id];
  setObjMatrix(v.pos);
  std::swap(v.timeShift, v2.timeShift);

  oldOw->free(oldId);
  }

void ObjectsBucket::Item::setFatness(float f) {
  if(owner!=nullptr)
    owner->setFatness(id,f);
  }

void ObjectsBucket::Item::setWind(phoenix::animation_mode m, float intensity) {
  if(owner!=nullptr)
    owner->setWind(id,m,intensity);
  }

void ObjectsBucket::Item::startMMAnim(std::string_view anim, float intensity, uint64_t timeUntil) {
  if(owner!=nullptr)
    owner->startMMAnim(id,anim,intensity,timeUntil);
  }

const Bounds& ObjectsBucket::Item::bounds() const {
  if(owner!=nullptr)
    return owner->bounds(id);
  static Bounds b;
  return b;
  }

void ObjectsBucket::Item::draw(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) const {
  owner->draw(id,p,fId);
  }


void ObjectsBucket::Descriptors::alloc(ObjectsBucket& owner) {
  auto& device = Resources::device();
  for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    if(owner.pMain!=nullptr)
      ubo[i][SceneGlobals::V_Main] = device.descriptors(owner.pMain->layout());
    if(owner.pShadow!=nullptr) {
      for(size_t lay=SceneGlobals::V_Shadow0; lay<=SceneGlobals::V_ShadowLast; ++lay)
        ubo[i][lay] = device.descriptors(owner.pShadow->layout());
      }
    }
  }


ObjectsBucket::ObjectsBucket(const Type type, const Material& mat, VisualObjects& owner, const SceneGlobals& scene,
                             const StaticMesh* stMesh, const AnimMesh* anim, const Tempest::StorageBuffer* desc)
  :objType(type), owner(owner), mat(mat), scene(scene) {
  static_assert(sizeof(UboPush)<=128, "UboPush is way too big");
  auto& device = Resources::device();

  instanceDesc = desc;
  staticMesh   = stMesh;
  animMesh     = anim;

  useSharedUbo        = (mat.frames.size()==0);
  textureInShadowPass = (mat.alpha==Material::AlphaTest);
  useMeshlets         = (Gothic::inst().doMeshShading() && !mat.isTesselated() && (type!=Type::Pfx));
  usePositionsSsbo    = (type==Type::Static || type==Type::Movable || type==Type::Morph);

  pMain    = Shaders::inst().materialPipeline(mat,objType,Shaders::T_Forward);
  pGbuffer = Shaders::inst().materialPipeline(mat,objType,Shaders::T_Deffered);
  pShadow  = Shaders::inst().materialPipeline(mat,objType,Shaders::T_Shadow);

  for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    uboBucket[i] = device.ubo<UboBucket>(nullptr,1);
    uboUpdateBucketDesc(i);
    }

  if(useSharedUbo) {
    uboShared.alloc(*this);
    uboSetCommon(uboShared,mat);
    }
  }

ObjectsBucket::~ObjectsBucket() {
  }

bool ObjectsBucket::isCompatible(const Type t, const Material& mat,
                                 const StaticMesh* st, const AnimMesh* ani,
                                 const Tempest::StorageBuffer* desc) const {
  auto type = sanitizeType(t,mat,st);
  if(objType!=type)
    return false;

  if(type==Pfx) {
    return mat==this->mat;
    }

  if(type==Landscape) {
    if(Gothic::inst().doMeshShading()) {
      return objType==type && mat.alpha==this->mat.alpha && desc==instanceDesc;
      }
    return mat==this->mat;
    }

  if(type==LandscapeShadow) {
    return false;
    }

  return this->mat==mat && instanceDesc==desc && staticMesh==st && animMesh==ani;
  }

std::unique_ptr<ObjectsBucket> ObjectsBucket::mkBucket(Type type, const Material& mat, VisualObjects& owner, const SceneGlobals& scene,
                                                       const StaticMesh* st, const AnimMesh* anim, const StorageBuffer* desc) {
  type = sanitizeType(type,mat,st);

  if(type==Landscape || type==LandscapeShadow)
    return std::unique_ptr<ObjectsBucket>(new ObjectsBucketDyn(type,mat,owner,scene,st,nullptr,desc));

  if(mat.frames.size()>0)
    return std::unique_ptr<ObjectsBucket>(new ObjectsBucketDyn(type,mat,owner,scene,st,anim,nullptr));

  return std::unique_ptr<ObjectsBucket>(new ObjectsBucket(type,mat,owner,scene,st,anim,nullptr));
  }

ObjectsBucket::Type ObjectsBucket::sanitizeType(const Type t, const Material& mat, const StaticMesh* st) {
  auto type = t;
  if(type==Landscape && mat.texAniMapDirPeriod!=Tempest::Point(0,0))
    type = Static;
  if(type==Landscape && mat.alpha==Material::Water)
    type = Static;
  if(type==Landscape && mat.frames.size()>0)
    type = Static;

  if(st!=nullptr && st->morph.anim!=nullptr) {
    type = ObjectsBucket::Morph;
    }
  return type;
  }

const Material& ObjectsBucket::material() const {
  return mat;
  }

const void* ObjectsBucket::meshPointer() const {
  if(staticMesh!=nullptr)
    return staticMesh;
  return animMesh;
  }

BufferHeap ObjectsBucket::ssboHeap() const {
  if(windAnim)
    return BufferHeap::Upload;
  auto heap = BufferHeap::Upload;
  if(objType==Type::Landscape || objType==Type::Static)
    heap = BufferHeap::Device;
  return heap;
  }

ObjectsBucket::Object& ObjectsBucket::implAlloc(const Bounds& bounds, const Material& /*mat*/) {
  Object* v = nullptr;
  for(size_t i=0; i<CAPACITY; ++i) {
    auto& vx = val[i];
    if(vx.isValid)
      continue;
    v = &vx;
    break;
    }

  if(valSz==0)
    owner.resetIndex();

  ++valSz;
  v->timeShift  = uint64_t(0-scene.tickCount);
  if(objType==Type::Landscape || objType==Type::Static)
    v->visibility = owner.visGroup.get(VisibilityGroup::G_Static);
  else if(objType==Type::LandscapeShadow) {
    v->visibility = owner.visGroup.get(VisibilityGroup::G_Default);
    }
  else if(objType==Type::Pfx)
    v->visibility = owner.visGroup.get(VisibilityGroup::G_AlwaysVis);
  else
    v->visibility = owner.visGroup.get(VisibilityGroup::G_Default);
  v->visibility.setBounds(bounds);
  v->visibility.setObject(&visSet,size_t(std::distance(val,v)));
  v->skiningAni = nullptr;
  v->isValid    = true;
  reallocObjPositions();

  return *v;
  }

void ObjectsBucket::postAlloc(Object& obj, size_t /*objId*/) {
  if(objType!=Pfx) {
    assert(obj.iboOffset%PackedMesh::MaxInd==0);
    assert(obj.iboLength%PackedMesh::MaxInd==0);
    }
  invalidateInstancing();
  }

void ObjectsBucket::implFree(const size_t objId) {
  auto& v = val[objId];
  if(v.blas!=nullptr)
    owner.resetTlas();

  v.visibility = VisibilityGroup::Token();
  v.isValid    = false;
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i)
    v.vboM[i] = nullptr;
  v.blas = nullptr;
  valSz--;
  visSet.erase(objId);

  if(valSz==0) {
    owner.resetIndex();
    objPositions = MatrixStorage::Id();
    } else {
    objPositions.set(dummyMatrix(),objId);
    }
  invalidateInstancing();
  }

void ObjectsBucket::uboSetCommon(Descriptors& v, const Material& mat) {
  for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    for(size_t lay=SceneGlobals::V_Shadow0; lay<SceneGlobals::V_Count; ++lay) {
      auto& ubo = v.ubo[i][lay];
      if(ubo.isEmpty())
        continue;
      ubo.set(L_Scene, scene.uboGlobalPf[i][lay]);
      if(useMeshlets && (objType==Type::Landscape || objType==Type::LandscapeShadow)) {
        ubo.set(L_MeshDesc, *instanceDesc);
        }
      if(objType!=ObjectsBucket::Landscape && objType!=ObjectsBucket::LandscapeShadow && objType!=ObjectsBucket::Pfx) {
        ubo.set(L_Material, uboBucket[i]);
        }
      if(useMeshlets) {
        if(staticMesh!=nullptr) {
          ubo.set(L_Vbo, staticMesh->vbo);
          ubo.set(L_Ibo, staticMesh->ibo);
          } else {
          ubo.set(L_Vbo, animMesh->vbo);
          ubo.set(L_Ibo, animMesh->ibo);
          }
        }
      if(lay==SceneGlobals::V_Main || textureInShadowPass) {
        ubo.set(L_Diffuse, *mat.tex);
        }
      if(lay==SceneGlobals::V_Main) {
        ubo.set(L_Shadow0,  *scene.shadowMap[0],Resources::shadowSampler());
        ubo.set(L_Shadow1,  *scene.shadowMap[1],Resources::shadowSampler());
        }
      if(objType==ObjectsBucket::Morph) {
        ubo.set(L_MorphId,  *staticMesh->morph.index  );
        ubo.set(L_Morph,    *staticMesh->morph.samples);
        }
      if(lay==SceneGlobals::V_Main && isSceneInfoRequired()) {
        auto smp = Sampler::bilinear();
        smp.setClamping(ClampMode::MirroredRepeat);
        ubo.set(L_GDiffuse, *scene.gbufEmission,smp);

        smp = Sampler::nearest();
        smp.setClamping(ClampMode::ClampToEdge);
        ubo.set(L_GDepth,   *scene.gbufDepth,  smp);
        }
      if(lay==SceneGlobals::V_Main && useMeshlets) {
        auto smp = Sampler::nearest();
        smp.setClamping(ClampMode::ClampToEdge);
        ubo.set(L_HiZ, *scene.hiZ, smp);
        }
      if(lay==SceneGlobals::V_Main && mat.alpha==Material::Water) {
        ubo.set(L_SkyLut, *scene.skyLut);
        }
      }
    uboSetSkeleton(v,i);
    }
  }

void ObjectsBucket::uboSetSkeleton(Descriptors& v, uint8_t fId) {
  auto& ssbo = owner.matrixSsbo(ssboHeap(),fId);
  if(ssbo.byteSize()==0 || objType==Type::Landscape || objType==Type::LandscapeShadow || objType==Type::Pfx)
    return;

  for(size_t lay=SceneGlobals::V_Shadow0; lay<SceneGlobals::V_Count; ++lay) {
    auto& ubo = v.ubo[fId][lay];
    if(!ubo.isEmpty())
      ubo.set(L_Matrix, ssbo);
    }
  }

void ObjectsBucket::uboSetDynamic(Descriptors& v, Object& obj, uint8_t fId) {
  auto& ubo = v.ubo[fId][SceneGlobals::V_Main];

  if(mat.frames.size()!=0 && mat.texAniFPSInv != 0) {
    auto frame = size_t((obj.timeShift+scene.tickCount)/mat.texAniFPSInv);
    auto t = mat.frames[frame%mat.frames.size()];
    ubo.set(L_Diffuse, *t);
    if(pShadow!=nullptr && textureInShadowPass) {
      for(size_t lay=SceneGlobals::V_Shadow0; lay<=SceneGlobals::V_ShadowLast; ++lay) {
        auto& uboSh = v.ubo[fId][lay];
        uboSh.set(L_Diffuse, *t);
        }
      }
    }
  }

void ObjectsBucket::uboUpdateBucketDesc(uint8_t fId) {
  UboBucket ubo;
  if(mat.texAniMapDirPeriod.x!=0) {
    uint64_t fract = scene.tickCount%uint64_t(std::abs(mat.texAniMapDirPeriod.x));
    ubo.texAniMapDir.x = float(fract)/float(mat.texAniMapDirPeriod.x);
    }
  if(mat.texAniMapDirPeriod.y!=0) {
    uint64_t fract = scene.tickCount%uint64_t(std::abs(mat.texAniMapDirPeriod.y));
    ubo.texAniMapDir.y = float(fract)/float(mat.texAniMapDirPeriod.y);
    }

  ubo.waveAnim = 2.f*float(M_PI)*float(scene.tickCount%3000)/3000.f;
  ubo.waveMaxAmplitude = mat.waveMaxAmplitude;

  if(staticMesh!=nullptr) {
    auto& bbox     = staticMesh->bbox.bbox;
    ubo.bboxRadius = staticMesh->bbox.rConservative;
    ubo.bbox[0]    = Vec4(bbox[0].x,bbox[0].y,bbox[0].z,0.f);
    ubo.bbox[1]    = Vec4(bbox[1].x,bbox[1].y,bbox[1].z,0.f);
    }
  if(animMesh!=nullptr) {
    auto& bbox     = animMesh->bbox.bbox;
    ubo.bboxRadius = animMesh->bbox.rConservative;
    ubo.bbox[0]    = Vec4(bbox[0].x,bbox[0].y,bbox[0].z,0.f);
    ubo.bbox[1]    = Vec4(bbox[1].x,bbox[1].y,bbox[1].z,0.f);
    }

  uboBucket[fId].update(&ubo,0,1);
  }

ObjectsBucket::Descriptors& ObjectsBucket::objUbo(size_t objId) {
  return uboShared;
  }

void ObjectsBucket::setupUbo() {
  uboSetCommon(uboShared,mat);
  }

void ObjectsBucket::invalidateUbo(uint8_t fId) {
  if(useSharedUbo)
    uboSetSkeleton(uboShared,fId);
  }

void ObjectsBucket::fillTlas(std::vector<RtInstance>& inst, std::vector<uint32_t>& iboOff, Bindless& out) {
  for(size_t i=0; i<CAPACITY; ++i) {
    auto& v = val[i];
    if(!v.isValid || v.blas==nullptr)
      continue;

    if(mat.tex!=out.tex.back() ||
       &staticMesh->vbo!=out.vbo.back() ||
       &staticMesh->ibo!=out.ibo.back() ||
       uint32_t(v.iboOffset/3)!=iboOff.back()) {
      out.tex.push_back(mat.tex);
      out.vbo.push_back(&staticMesh->vbo);
      out.ibo.push_back(&staticMesh->ibo);
      iboOff.push_back(uint32_t(v.iboOffset/3));
      }

    RtInstance ix;
    ix.mat  = v.pos;
    ix.id   = uint32_t(out.tex.size()-1);
    ix.blas = v.blas;
    inst.push_back(ix);
    }
  }

void ObjectsBucket::preFrameUpdate(uint8_t fId) {
  if(mat.texAniMapDirPeriod.x!=0 || mat.texAniMapDirPeriod.y!=0 || mat.alpha==Material::Water) {
    uboUpdateBucketDesc(fId);
    }

  if(windAnim && scene.zWindEnabled) {
    bool upd[CAPACITY] = {};
    for(uint8_t ic=0; ic<SceneGlobals::V_Count; ++ic) {
      const auto    c     = SceneGlobals::VisCamera(ic);
      const size_t  indSz = visSet.count(c);
      const size_t* index = visSet.index(c);
      for(size_t i=0; i<indSz; ++i)
        upd[index[i]] = true;
      }

    for(size_t i=0; i<CAPACITY; ++i) {
      auto& v = val[i];
      if(upd[i] && v.wind!=phoenix::animation_mode::none) {
        auto pos = v.pos;
        float shift = v.pos[3][0]*scene.windDir.x + v.pos[3][2]*scene.windDir.y;

        static const uint64_t preiod = scene.windPeriod;
        float a = float(scene.tickCount%preiod)/float(preiod);
        a = a*2.f-1.f;
        a = std::cos(float(a*M_PI) + shift*0.0001f);

        switch(v.wind) {
          case phoenix::animation_mode::wind:
            // tree
            // a *= v.windIntensity;
            a *= 0.03f;
            break;
          case phoenix::animation_mode::wind2:
            // grass
            // a *= v.windIntensity;
            a *= 0.0005f;
            break;
          case phoenix::animation_mode::none:
          default:
            // error
            a *= 0.f;
            break;
          }
        pos[1][0] += scene.windDir.x*a;
        pos[1][2] += scene.windDir.y*a;
        objPositions.set(pos,i);
        }
      }
    }
  }

size_t ObjectsBucket::alloc(const StaticMesh& mesh, size_t iboOffset, size_t iboLen,
                            const Bounds& bounds, const Material& mat) {
  Object* v = &implAlloc(bounds,mat);
  v->iboOffset = iboOffset;
  v->iboLength = iboLen;

  bool useBlas = true;
  if(mat.alpha==Material::Solid && objType==Type::Landscape)
    useBlas = false; // handles separetly

  if(objType!=Type::Landscape && objType!=Type::Static)
    useBlas = false; // not supported
  if(mat.isGhost)
    useBlas = false;
  if(mat.alpha!=Material::Solid && mat.alpha!=Material::AlphaTest && mat.alpha!=Material::Transparent)
    useBlas = false;

  if(useBlas) {
    for(auto& i:mesh.sub)
      if(i.iboOffset==iboOffset && i.iboLength==iboLen && !i.blas.isEmpty()) {
        v->blas = &i.blas;
        owner.resetTlas();
        break;
        }
    }
  postAlloc(*v,size_t(std::distance(val,v)));
  return size_t(std::distance(val,v));
  }

size_t ObjectsBucket::alloc(const AnimMesh& mesh, size_t iboOffset, size_t iboLen,
                            const MatrixStorage::Id& anim) {
  Object* v = &implAlloc(mesh.bbox,mat);
  v->iboOffset  = iboOffset;
  v->iboLength  = iboLen;
  v->skiningAni = &anim;
  postAlloc(*v,size_t(std::distance(val,v)));
  return size_t(std::distance(val,v));
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<ObjectsBucket::Vertex>* vbo[], const Bounds& bounds) {
  Object* v = &implAlloc(bounds,mat);
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    assert(vbo[i]);
    v->vboM[i] = vbo[i];
    }
  v->visibility.setGroup(VisibilityGroup::G_AlwaysVis);
  postAlloc(*v,size_t(std::distance(val,v)));
  return size_t(std::distance(val,v));
  }

void ObjectsBucket::free(const size_t objId) {
  implFree(objId);
  }

void ObjectsBucket::draw(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  if(pMain==nullptr)
    return;
  drawCommon(cmd,fId,*pMain,SceneGlobals::V_Main,false);
  }

void ObjectsBucket::drawGBuffer(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  if(pGbuffer==nullptr)
    return;
  drawCommon(cmd,fId,*pGbuffer,SceneGlobals::V_Main,false);
  }

void ObjectsBucket::drawShadow(Encoder<CommandBuffer>& cmd, uint8_t fId, int layer) {
  if(pShadow==nullptr)
    return;
  drawCommon(cmd,fId,*pShadow,SceneGlobals::VisCamera(SceneGlobals::V_Shadow0+layer),false);
  }

void ObjectsBucket::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& /*cmd*/, uint8_t /*fId*/) {
  }

void ObjectsBucket::drawCommon(Encoder<CommandBuffer>& cmd, uint8_t fId, const RenderPipeline& shader,
                               SceneGlobals::VisCamera c, bool isHiZPass) {
  const size_t  indSz = visSet.count(c);
  const size_t* index = visSet.index(c);
  if(indSz==0)
    return;

  if(useMeshlets) {
    if(objType==Landscape && c==SceneGlobals::V_Shadow1 && !textureInShadowPass)
      return;
    if(objType==LandscapeShadow && c!=SceneGlobals::V_Shadow1)
      return;
    }

  if(instancingType==Normal)
    visSet.sort(c);
  else if(instancingType==Aggressive)
    visSet.minmax(c);

  cmd.setUniforms(shader, uboShared.ubo[fId][c]);
  UboPush pushBlock = {};
  for(size_t i=0; i<indSz; ++i) {
    auto  id = index[i];
    auto& v  = val[id];

    switch(objType) {
      case Landscape:
      case LandscapeShadow: {
        if(useMeshlets) {
          cmd.dispatchMesh(v.iboOffset/PackedMesh::MaxInd, v.iboLength/PackedMesh::MaxInd);
          } else {
          cmd.draw(staticMesh->vbo, staticMesh->ibo, v.iboOffset, v.iboLength);
          }
        break;
        }
      case Pfx: {
        cmd.draw(*v.vboM[fId]);
        break;
        }
      case Static:
      case Movable:
      case Morph:
      case Animated: {
        uint32_t instance = 0;
        if(objType!=Animated)
          instance = objPositions.offsetId()+uint32_t(id); else
          instance = v.skiningAni->offsetId();

        uint32_t cnt   = applyInstancing(i,index,indSz);
        size_t   uboSz = (objType==Morph ? sizeof(UboPush) : sizeof(UboPushBase));

        updatePushBlock(pushBlock,v);
        if(useMeshlets) {
          pushBlock.meshletBase  = uint32_t(v.iboOffset/PackedMesh::MaxInd);
          pushBlock.meshletCount = uint32_t(v.iboLength/PackedMesh::MaxInd);
          cmd.setUniforms(shader, &pushBlock, uboSz);
          cmd.dispatchMesh(instance*pushBlock.meshletCount, cnt*pushBlock.meshletCount);
          } else {
          cmd.setUniforms(shader, &pushBlock, uboSz);
          if(objType!=Animated)
            cmd.draw(staticMesh->vbo, staticMesh->ibo, v.iboOffset, v.iboLength, instance, cnt); else
            cmd.draw(animMesh  ->vbo, animMesh  ->ibo, v.iboOffset, v.iboLength, instance, cnt);
          }
        break;
        }
      }
    }
  }

void ObjectsBucket::draw(size_t id, Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  auto& v = val[id];
  if(staticMesh==nullptr || pMain==nullptr)
    return;

  auto& ubo = objUbo(id);
  uboSetDynamic(ubo,v,fId);

  UboPush pushBlock = {};
  cmd.setUniforms(*pMain,ubo.ubo[fId][SceneGlobals::V_Main]);
  switch(objType) {
    case Landscape:
    case LandscapeShadow:
    case Animated:
    case Pfx:
      break;
    case Static:
    case Movable:
    case Morph: {
      uint32_t instance = objPositions.offsetId()+uint32_t(id);
      size_t   uboSz    = (objType==Morph ? sizeof(UboPush) : sizeof(UboPushBase));

      updatePushBlock(pushBlock,v);
      if(useMeshlets) {
        pushBlock.meshletBase  = uint32_t(v.iboOffset/PackedMesh::MaxInd);
        pushBlock.meshletCount = uint32_t(v.iboLength/PackedMesh::MaxInd);
        cmd.setUniforms(*pMain, &pushBlock, uboSz);
        cmd.dispatchMesh(instance*pushBlock.meshletCount, 1*pushBlock.meshletCount);
        } else {
        cmd.setUniforms(*pMain, &pushBlock, uboSz);
        cmd.draw(staticMesh->vbo, staticMesh->ibo, v.iboOffset, v.iboLength, instance, 1);
        }
      break;
      }
    }
  }

void ObjectsBucket::setObjMatrix(size_t i, const Matrix4x4& m) {
  auto& v = val[i];
  v.visibility.setObjMatrix(m);
  v.pos = m;

  if(objPositions.size()>0)
    objPositions.set(m,i);

  if(v.blas!=nullptr)
    owner.resetTlas();
  }

void ObjectsBucket::setBounds(size_t i, const Bounds& b) {
  val[i].visibility.setBounds(b);
  }

void ObjectsBucket::startMMAnim(size_t i, std::string_view animName, float intensity, uint64_t timeUntil) {
  if(staticMesh==nullptr || staticMesh->morph.anim==nullptr)
    return;
  auto&  anim = *staticMesh->morph.anim;
  auto&  v    = val[i];
  size_t id   = size_t(-1);
  for(size_t i=0; i<anim.size(); ++i)
    if(anim[i].name==animName) {
      id = i;
      break;
      }

  if(id==size_t(-1))
    return;

  auto& m = anim[id];
  if(timeUntil==uint64_t(-1) && m.duration>0)
    timeUntil = scene.tickCount + m.duration;

  // extend time of anim
  for(auto& i:v.morphAnim) {
    if(i.id!=id || i.timeUntil<scene.tickCount)
      continue;
    i.timeUntil = timeUntil;
    i.intensity = intensity;
    return;
    }

  // find same layer
  for(auto& i:v.morphAnim) {
    if(i.timeUntil<scene.tickCount)
      continue;
    if(anim[i.id].layer!=m.layer)
      continue;
    i.id        = id;
    i.timeUntil = timeUntil;
    i.intensity = intensity;
    return;
    }

  size_t nId = 0;
  for(size_t i=0; i<Resources::MAX_MORPH_LAYERS; ++i) {
    if(v.morphAnim[nId].timeStart<=v.morphAnim[i].timeStart)
      continue;
    nId = i;
    }

  auto& ani = v.morphAnim[nId];
  ani.id        = id;
  ani.timeStart = scene.tickCount;
  ani.timeUntil = timeUntil;
  ani.intensity = intensity;
  }

void ObjectsBucket::setFatness(size_t i, float f) {
  auto& v = val[i];
  v.fatness = f*0.5f;
  invalidateInstancing();
  }

void ObjectsBucket::setWind(size_t i, phoenix::animation_mode m, float intensity) {
  auto& v = val[i];
  v.wind          = m;
  v.windIntensity = intensity;
  reallocObjPositions();
  }

bool ObjectsBucket::isSceneInfoRequired() const {
  return mat.isGhost || mat.alpha==Material::Water || mat.alpha==Material::Ghost;
  }

void ObjectsBucket::updatePushBlock(ObjectsBucket::UboPush& push, ObjectsBucket::Object& v) {
  push.fatness = v.fatness;

  if(objType==Morph) {
    for(size_t i=0; i<Resources::MAX_MORPH_LAYERS; ++i) {
      auto&    ani  = v.morphAnim[i];
      auto&    anim = (*staticMesh->morph.anim)[ani.id];
      uint64_t time = (scene.tickCount-ani.timeStart);

      float alpha     = float(time%anim.tickPerFrame)/float(anim.tickPerFrame);
      float intensity = ani.intensity;

      if(scene.tickCount>ani.timeUntil)
        intensity = 0;

      const uint32_t samplesPerFrame = uint32_t(anim.samplesPerFrame);
      push.morph[i].indexOffset = uint32_t(anim.index);
      push.morph[i].sample0     = uint32_t((time/anim.tickPerFrame+0)%anim.numFrames)*samplesPerFrame;
      push.morph[i].sample1     = uint32_t((time/anim.tickPerFrame+1)%anim.numFrames)*samplesPerFrame;
      push.morph[i].alpha       = uint16_t(alpha*uint16_t(-1));
      push.morph[i].intensity   = uint16_t(intensity*uint16_t(-1));
      }
    }
  }

void ObjectsBucket::reallocObjPositions() {
  if(usePositionsSsbo) {
    windAnim = false;
    for(size_t i=0; i<CAPACITY; ++i) {
      auto& vx = val[i];
      if(vx.isValid && vx.wind!=phoenix::animation_mode::none) {
        windAnim = true;
        break;
        }
      }

    size_t valLen = 1;
    for(size_t i=CAPACITY; i>1; --i)
      if(val[i-1].isValid) {
        valLen = i;
        break;
        }

    auto sz   = nextPot(uint32_t(valLen));
    auto heap = ssboHeap();
    if(objPositions.size()!=sz || heap!=objPositions.heap()) {
      objPositions = owner.getMatrixes(heap, sz);
      for(size_t i=0; i<valLen; ++i)
        objPositions.set(val[i].pos,i);
      }
    }
  }

void ObjectsBucket::invalidateInstancing() {
  instancingType = Normal;
  if(!useSharedUbo || objType==Pfx) {
    instancingType = NoInstancing;
    return;
    }
  Object* pref = nullptr;
  for(size_t i=0; instancingType!=NoInstancing && i<CAPACITY; ++i) {
    auto& vx = val[i];
    if(!vx.isValid)
      continue;
    if(pref==nullptr)
      pref = &vx;

    auto& ref = *pref;
    if(vx.iboOffset!=ref.iboOffset || vx.iboLength!=ref.iboLength)
      instancingType = NoInstancing;
    if(vx.fatness!=ref.fatness)
      instancingType = NoInstancing;
    if(vx.skiningAni!=ref.skiningAni)
      instancingType = NoInstancing;
    }

  if(instancingType==Normal && pref!=nullptr) {
    if(staticMesh!=nullptr && staticMesh->ibo.size()<=PackedMesh::MaxInd){
      instancingType = Aggressive;
      }
    if(useMeshlets && animMesh==nullptr) {
      instancingType = Aggressive;
      }
    }
  }

uint32_t ObjectsBucket::applyInstancing(size_t& i, const size_t* index, size_t indSz) const {
  if(instancingType==NoInstancing) {
    return 1;
    }
  if(instancingType==Aggressive) {
    assert(i==0);
    auto ret = uint32_t(index[indSz-1]-index[0]+1);
    i = indSz;
    return ret;
    }
  auto     id = index[i];
  uint32_t cnt = 1;
  while(i+1<indSz) {
    if(index[i+1]!=id+cnt)
      break;
    ++cnt;
    ++i;
    }
  return cnt;
  }

const Bounds& ObjectsBucket::bounds(size_t i) const {
  return val[i].visibility.bounds();
  }


ObjectsBucketDyn::ObjectsBucketDyn(const Type type, const Material& mat, VisualObjects& owner, const SceneGlobals& scene,
                                   const StaticMesh* st, const AnimMesh* anim, const Tempest::StorageBuffer* desc)
  :ObjectsBucket(type,mat,owner,scene,st,anim,desc) {
  if(useMeshlets && objType==Type::LandscapeShadow) {
    auto& device = Resources::device();
    pHiZ = &Shaders::inst().lndPrePass;
    for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i) {
      auto& ubo = uboHiZ.ubo[i][SceneGlobals::V_Shadow1];
      ubo = device.descriptors(pHiZ->layout());
      if(ubo.isEmpty())
        continue;
      ubo.set(L_MeshDesc, *instanceDesc);
      ubo.set(L_Vbo,      staticMesh->vbo);
      ubo.set(L_Ibo,      staticMesh->ibo);
      ubo.set(L_Scene,    scene.uboGlobalPf[i][SceneGlobals::V_Main]);

      auto smp = Sampler::nearest();
      smp.setClamping(ClampMode::ClampToEdge);
      ubo.set(L_HiZ,      *scene.hiZ, smp);
      }
    }
  }

void ObjectsBucketDyn::preFrameUpdate(uint8_t fId) {
  ObjectsBucket::preFrameUpdate(fId);

  if(!hasDynMaterials)
    return;

  for(uint8_t ic=0; ic<SceneGlobals::V_Count; ++ic) {
    const auto    c     = SceneGlobals::VisCamera(ic);
    const size_t  indSz = visSet.count(c);
    const size_t* index = visSet.index(c);
    for(size_t i=0; i<indSz; ++i) {
      auto& v = val[index[i]];
      uboSetDynamic(uboObj[index[i]],v,fId);
      }
    }
  }

ObjectsBucket::Object& ObjectsBucketDyn::implAlloc(const Bounds& bounds, const Material& m) {
  auto& obj = ObjectsBucket::implAlloc(bounds,m);

  const size_t id = size_t(std::distance(val,&obj));
  mat[id] = m;
  uboObj[id].alloc(*this);
  uboSetCommon(uboObj[id],mat[id]);

  invalidateDyn();
  return obj;
  }

void ObjectsBucketDyn::implFree(const size_t objId) {
  for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i)
    for(uint8_t lay=SceneGlobals::V_Shadow0; lay<SceneGlobals::V_Count; ++lay) {
      auto& set = uboObj[objId].ubo[i][lay];
      owner.recycle(std::move(set));
      }
  ObjectsBucket::implFree(objId);
  invalidateDyn();
  }

ObjectsBucket::Descriptors& ObjectsBucketDyn::objUbo(size_t objId) {
  return uboObj[objId];
  }

void ObjectsBucketDyn::setupUbo() {
  ObjectsBucket::setupUbo();

  for(size_t i=0; i<CAPACITY; ++i) {
    if(!uboObj[i].ubo[0][SceneGlobals::V_Main].isEmpty())
      uboSetCommon(uboObj[i],mat[i]);
    }

  if(pHiZ!=nullptr) {
    for(uint8_t i=0; i<Resources::MaxFramesInFlight; ++i) {
      auto& ubo = uboHiZ.ubo[i][SceneGlobals::V_Shadow1];
      if(ubo.isEmpty())
        continue;
      auto smp = Sampler::nearest();
      smp.setClamping(ClampMode::ClampToEdge);
      ubo.set(L_HiZ,      *scene.hiZ, smp);
      }
    }
  }

void ObjectsBucketDyn::invalidateUbo(uint8_t fId) {
  ObjectsBucket::invalidateUbo(fId);

  for(auto& v:uboObj)
    uboSetSkeleton(v,fId);
  }

void ObjectsBucketDyn::fillTlas(std::vector<Tempest::RtInstance>& inst, std::vector<uint32_t>& iboOff, Bindless& out) {
  for(size_t i=0; i<CAPACITY; ++i) {
    auto& v = val[i];
    if(!v.isValid || v.blas==nullptr)
      continue;

    if(mat[i].tex!=out.tex.back() ||
       &staticMesh->vbo!=out.vbo.back() ||
       &staticMesh->ibo!=out.ibo.back() ||
       uint32_t(v.iboOffset/3)!=iboOff.back()) {
      out.tex.push_back(mat[i].tex);
      out.vbo.push_back(&staticMesh->vbo);
      out.ibo.push_back(&staticMesh->ibo);
      iboOff.push_back(uint32_t(v.iboOffset/3));
      }

    RtInstance ix;
    ix.mat  = v.pos;
    ix.id   = uint32_t(out.tex.size()-1);
    ix.blas = v.blas;
    inst.push_back(ix);
    }
  }

void ObjectsBucketDyn::invalidateDyn() {
  hasDynMaterials = false;
  for(auto& v:val) {
    if(!v.isValid)
      continue;
    hasDynMaterials |= (mat->frames.size()>0);
    }
  }

void ObjectsBucketDyn::drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId,
                                  const Tempest::RenderPipeline& shader,
                                  SceneGlobals::VisCamera c, bool isHiZPass) {
  const size_t  indSz = visSet.count(c);
  const size_t* index = visSet.index(c);
  if(indSz==0)
    return;

  if(useMeshlets && !textureInShadowPass) {
    const bool isShadow = (c==SceneGlobals::V_Shadow1 || isHiZPass);
    if(objType==Landscape && isShadow)
      return;
    if(objType==LandscapeShadow && !isShadow)
      return;
    }

  UboPush pushBlock  = {};
  for(size_t i=0; i<indSz; ++i) {
    auto  id = index[i];
    auto& v  = val[id];

    switch(objType) {
      case Landscape:
      case LandscapeShadow: {
        cmd.setUniforms(shader, uboObj[id].ubo[fId][c]);
        if(useMeshlets) {
          cmd.dispatchMesh(v.iboOffset/PackedMesh::MaxInd, v.iboLength/PackedMesh::MaxInd);
          } else {
          cmd.draw(staticMesh->vbo, staticMesh->ibo, v.iboOffset, v.iboLength);
          }
        break;
        }
      case Pfx: {
        cmd.setUniforms(shader, uboObj[id].ubo[fId][c]);
        cmd.draw(*v.vboM[fId]);
        break;
        }
      case Static:
      case Movable:
      case Morph:
      case Animated:  {
        uint32_t instance = 0;
        if(objType!=Animated)
          instance = objPositions.offsetId()+uint32_t(id); else
          instance = v.skiningAni->offsetId();
        size_t   uboSz    = (objType==Morph ? sizeof(UboPush) : sizeof(UboPushBase));

        updatePushBlock(pushBlock,v);
        if(useMeshlets) {
          pushBlock.meshletBase  = uint32_t(v.iboOffset/PackedMesh::MaxInd);
          pushBlock.meshletCount = uint32_t(v.iboLength/PackedMesh::MaxInd);
          cmd.setUniforms(shader, uboObj[id].ubo[fId][c], &pushBlock, uboSz);
          cmd.dispatchMesh(instance*pushBlock.meshletCount, 1*pushBlock.meshletCount);
          } else {
          cmd.setUniforms(shader, uboObj[id].ubo[fId][c], &pushBlock, uboSz);
          if(objType!=Animated)
            cmd.draw(staticMesh->vbo, staticMesh->ibo, v.iboOffset, v.iboLength, instance, 1); else
            cmd.draw(animMesh  ->vbo, animMesh  ->ibo, v.iboOffset, v.iboLength, instance, 1);
          }
        break;
        }
      }
    }
  }

void ObjectsBucketDyn::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  if(pHiZ==nullptr || objType!=LandscapeShadow || !useMeshlets)
    return;
  cmd.setUniforms(*pHiZ, uboHiZ.ubo[fId][SceneGlobals::V_Shadow1]);
  for(size_t i=0; i<valSz; ++i) {
    auto& v = val[i];
    if(!v.isValid)
      continue;
    cmd.dispatchMesh(v.iboOffset/PackedMesh::MaxInd, v.iboLength/PackedMesh::MaxInd);
    }
  }

// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh4_intersector1.h"
#include "bvh4_intersector_node.h"

#include "../geometry/triangle4.h"
#include "../geometry/triangle4v.h"
#include "../geometry/triangle4v_mb.h"
#include "../geometry/triangle4i.h"
#include "../geometry/triangle8.h"
#include "../geometry/intersector_iterators.h"
#include "../geometry/bezier1v_intersector.h"
#include "../geometry/bezier1i_intersector.h"
#include "../geometry/triangle_intersector_moeller.h"
#include "../geometry/triangle_intersector_pluecker.h"
#include "../geometry/triangle4i_intersector_pluecker.h"
#include "../geometry/subdivpatch1_intersector1.h"
#include "../geometry/subdivpatch1cached_intersector1.h"
#include "../geometry/grid_aos_intersector1.h"
#include "../geometry/object_intersector1.h"

namespace embree
{ 
  namespace isa
  {
    int getISA() { 
      return VerifyMultiTargetLinking::getISA(); 
    }

    template<int types, bool robust, typename PrimitiveIntersector>
    void BVH4Intersector1<types,robust,PrimitiveIntersector>::intersect(const BVH4* bvh, Ray& ray)
    {
      struct TravRay 
      {
        __forceinline TravRay(const Vec3fa& ray_org, const Vec3fa& ray_dir) 
          : org_xyz(ray_org), dir_xyz(ray_dir) 
        {
          const Vec3fa ray_rdir = rcp_safe(ray_dir);
          const Vec3fa ray_org_rdir = ray_org*ray_rdir;
          org = Vec3f4(ray_org.x,ray_org.y,ray_org.z);
          dir = Vec3f4(ray_dir.x,ray_dir.y,ray_dir.z);
          rdir = Vec3f4(ray_rdir.x,ray_rdir.y,ray_rdir.z);
          org_rdir = Vec3f4(ray_org_rdir.x,ray_org_rdir.y,ray_org_rdir.z);
          nearX = ray_rdir.x >= 0.0f ? 0*sizeof(float4) : 1*sizeof(float4);
          nearY = ray_rdir.y >= 0.0f ? 2*sizeof(float4) : 3*sizeof(float4);
          nearZ = ray_rdir.z >= 0.0f ? 4*sizeof(float4) : 5*sizeof(float4);
          farX  = nearX ^ sizeof(float4);
          farY  = nearY ^ sizeof(float4);
          farZ  = nearZ ^ sizeof(float4);
        }
        Vec3fa org_xyz, dir_xyz;
        Vec3f4 org, dir, rdir, org_rdir; // FIXME: is org_rdir optimized away?
        size_t nearX, nearY, nearZ;
        size_t farX, farY, farZ;
        int geomID;
        int instID;
      };

      /*! perform per ray precalculations required by the primitive intersector */
      Precalculations pre(ray,bvh);

      /*! stack state */
      StackItemT<NodeRef> stack[stackSize];            //!< stack of nodes 
      StackItemT<NodeRef>* stackPtr = stack+1;        //!< current stack pointer
      StackItemT<NodeRef>* stackEnd = stack+stackSize;
      stack[0].ptr  = bvh->root;
      stack[0].dist = neg_inf;

      /* filter out invalid rays */
#if defined(RTCORE_IGNORE_INVALID_RAYS)
      if (!ray.valid()) return;
#endif

      /* verify correct input */
      assert(ray.tnear > -FLT_MIN);
      assert(!(types & BVH4::FLAG_NODE_MB) || (ray.time >= 0.0f && ray.time <= 1.0f));

      /*! load the ray into SIMD registers */
      TravRay vray(ray.org,ray.dir);
      __aligned(32) char tlray[sizeof(TravRay)];
      new (tlray) TravRay(vray);
      float4 ray_near(ray.tnear);
      float4 ray_far (ray.tfar);

      /* pop loop */
      while (true) pop:
      {
        /*! pop next node */
        if (unlikely(stackPtr == stack)) break;
        stackPtr--;
        NodeRef cur = NodeRef(stackPtr->ptr);
        
        /*! if popped node is too far, pop next one */
        if (unlikely(*(float*)&stackPtr->dist > ray.tfar))
          continue;
        
        /* downtraversal loop */
        while (true)
        {
	  size_t mask; 
	  float4 tNear;

	  /*! stop if we found a leaf node */
	  if (unlikely(cur.isLeaf(types))) break;
          STAT3(normal.trav_nodes,1,1,1);

	  /* process standard nodes */
          if (likely(cur.isNode(types)))
	    mask = intersect_node<robust>(cur.node(),vray.nearX,vray.nearY,vray.nearZ,vray.farX,vray.farY,vray.farZ,vray.org,vray.rdir,vray.org_rdir,ray_near,ray_far,tNear); 

	  /* process motion blur nodes */
	  else if (likely(cur.isNodeMB(types)))
	    mask = intersect_node(cur.nodeMB(),vray.nearX,vray.nearY,vray.nearZ,vray.org,vray.rdir,vray.org_rdir,ray_near,ray_far,ray.time,tNear); 

	  /*! process nodes with unaligned bounds */
          else if (unlikely(cur.isUnalignedNode(types)))
            mask = intersect_node(cur.unalignedNode(),vray.org,vray.dir,ray_near,ray_far,tNear);

          /*! process nodes with unaligned bounds and motion blur */
          else if (unlikely(cur.isUnalignedNodeMB(types)))
            mask = intersect_node(cur.unalignedNodeMB(),vray.org,vray.dir,ray_near,ray_far,ray.time,tNear);

          else assert(false);

          /*! if no child is hit, pop next node */
	  const BVH4::BaseNode* node = cur.baseNode(types);
          if (unlikely(mask == 0))
            goto pop;
          
          /*! one child is hit, continue with that child */
	  size_t r = __bscf(mask);
	  if (likely(mask == 0)) {
            cur = node->child(r); cur.prefetch(types);
            assert(cur != BVH4::emptyNode);
            continue;
          }
          
          /*! two children are hit, push far child, and continue with closer child */
          NodeRef c0 = node->child(r); c0.prefetch(types); const unsigned int d0 = ((unsigned int*)&tNear)[r];
          r = __bscf(mask);
          NodeRef c1 = node->child(r); c1.prefetch(types); const unsigned int d1 = ((unsigned int*)&tNear)[r];
          assert(c0 != BVH4::emptyNode);
          assert(c1 != BVH4::emptyNode);
          if (likely(mask == 0)) {
            assert(stackPtr < stackEnd); 
            if (d0 < d1) { stackPtr->ptr = c1; stackPtr->dist = d1; stackPtr++; cur = c0; continue; }
            else         { stackPtr->ptr = c0; stackPtr->dist = d0; stackPtr++; cur = c1; continue; }
          }
          
          /*! Here starts the slow path for 3 or 4 hit children. We push
           *  all nodes onto the stack to sort them there. */
          assert(stackPtr < stackEnd); 
          stackPtr->ptr = c0; stackPtr->dist = d0; stackPtr++;
          assert(stackPtr < stackEnd); 
          stackPtr->ptr = c1; stackPtr->dist = d1; stackPtr++;
          
          /*! three children are hit, push all onto stack and sort 3 stack items, continue with closest child */
          assert(stackPtr < stackEnd); 
          r = __bscf(mask);
          NodeRef c = node->child(r); c.prefetch(types); unsigned int d = ((unsigned int*)&tNear)[r]; stackPtr->ptr = c; stackPtr->dist = d; stackPtr++;

          assert(c != BVH4::emptyNode);
          if (likely(mask == 0)) {
            sort(stackPtr[-1],stackPtr[-2],stackPtr[-3]);
            cur = (NodeRef) stackPtr[-1].ptr; stackPtr--;
            continue;
          }
          
          /*! four children are hit, push all onto stack and sort 4 stack items, continue with closest child */
          assert(stackPtr < stackEnd); 
          r = __bscf(mask);
          c = node->child(r); c.prefetch(types); d = *(unsigned int*)&tNear[r]; stackPtr->ptr = c; stackPtr->dist = d; stackPtr++;
          assert(c != BVH4::emptyNode);
          sort(stackPtr[-1],stackPtr[-2],stackPtr[-3],stackPtr[-4]);
          cur = (NodeRef) stackPtr[-1].ptr; stackPtr--;
        }

        /*! process transformation nodes */
        if (unlikely(cur.isTransformNode(types))) 
        {
          const BVH4::TransformNode* node = cur.transformNode();
          const Vec3fa ray_org = xfmPoint (node->world2local,((TravRay&)tlray).org_xyz);
          const Vec3fa ray_dir = xfmVector(node->world2local,((TravRay&)tlray).dir_xyz);
          new (&vray) TravRay(ray_org,ray_dir);
          ((TravRay&)tlray).geomID = ray.geomID;
          ((TravRay&)tlray).instID = ray.instID;
          ray.org = ray_org;
          ray.dir = ray_dir;
          ray.geomID = -1;
          ray.instID = node->instID;
          stackPtr->ptr = BVH4::popRay; stackPtr->dist = neg_inf; stackPtr++; // FIXME: requires larger stack!
          stackPtr->ptr = node->child;  stackPtr->dist = neg_inf; stackPtr++;
          goto pop;
        }

        /*! restore toplevel ray */
        if (cur == BVH4::popRay) {
          vray = (TravRay&) tlray; 
          ray.org = ((TravRay&)tlray).org_xyz;
          ray.dir = ((TravRay&)tlray).dir_xyz;
          if (ray.geomID == -1) {
            ray.geomID = ((TravRay&)tlray).geomID;
            ray.instID = ((TravRay&)tlray).instID;
          }
          goto pop;
        }
        
        /*! this is a leaf node */
	assert(cur != BVH4::emptyNode);
        STAT3(normal.trav_leaves,1,1,1);
        size_t num; Primitive* prim = (Primitive*) cur.leaf(num);
        size_t lazy_node = 0;
        PrimitiveIntersector::intersect(pre,ray,prim,num,bvh->scene,lazy_node);
        ray_far = ray.tfar;

        if (unlikely(lazy_node)) {
          stackPtr->ptr = lazy_node;
          stackPtr->dist = neg_inf;
          stackPtr++;
        }
      }
      AVX_ZERO_UPPER();
    }
    
    template<int types, bool robust, typename PrimitiveIntersector>
    void BVH4Intersector1<types,robust,PrimitiveIntersector>::occluded(const BVH4* bvh, Ray& ray)
    {
      /*! perform per ray precalculations required by the primitive intersector */
      Precalculations pre(ray,bvh);

      /*! stack state */
      NodeRef stack[stackSize];  //!< stack of nodes that still need to get traversed
      NodeRef* stackPtr = stack+1;        //!< current stack pointer
      NodeRef* stackEnd = stack+stackSize;
      stack[0] = bvh->root;
      
      /* filter out invalid rays */
#if defined(RTCORE_IGNORE_INVALID_RAYS)
      if (!ray.valid()) return;
#endif

      /* verify correct input */
      assert(ray.tnear > -FLT_MIN);
      assert(!(types & BVH4::FLAG_NODE_MB) || (ray.time >= 0.0f && ray.time <= 1.0f));

      /*! load the ray into SIMD registers */
      const Vec3fa ray_rdir = rcp_safe(ray.dir);
      const Vec3fa ray_org_rdir = ray.org*ray_rdir;
      const Vec3f4 org(ray.org.x,ray.org.y,ray.org.z);
      const Vec3f4 dir(ray.dir.x,ray.dir.y,ray.dir.z);
      const Vec3f4 rdir(ray_rdir.x,ray_rdir.y,ray_rdir.z);
      const Vec3f4 org_rdir(ray_org_rdir.x,ray_org_rdir.y,ray_org_rdir.z);
      const float4  ray_near(ray.tnear);
      float4 ray_far(ray.tfar);

      /*! offsets to select the side that becomes the lower or upper bound */
      const size_t nearX = ray_rdir.x >= 0 ? 0*sizeof(float4) : 1*sizeof(float4);
      const size_t nearY = ray_rdir.y >= 0 ? 2*sizeof(float4) : 3*sizeof(float4);
      const size_t nearZ = ray_rdir.z >= 0 ? 4*sizeof(float4) : 5*sizeof(float4);      
      
      /* pop loop */
      while (true) pop:
      {
        /*! pop next node */
        if (unlikely(stackPtr == stack)) break;
        stackPtr--;
        NodeRef cur = (NodeRef) *stackPtr;
        
        /* downtraversal loop */
        while (true)
        {
	  size_t mask; 
	  float4 tNear;

	  /*! stop if we found a leaf node */
	  if (unlikely(cur.isLeaf(types))) break;
	  STAT3(shadow.trav_nodes,1,1,1);

	  /* process standard nodes */
          if (likely(cur.isNode(types)))
	    mask = intersect_node<robust>(cur.node(),nearX,nearY,nearZ,org,rdir,org_rdir,ray_near,ray_far,tNear); 

	  /* process motion blur nodes */
	  else if (likely(cur.isNodeMB(types)))
	    mask = intersect_node(cur.nodeMB(),nearX,nearY,nearZ,org,rdir,org_rdir,ray_near,ray_far,ray.time,tNear); 

	  /*! process nodes with unaligned bounds */
          else if (unlikely(cur.isUnalignedNode(types)))
            mask = intersect_node(cur.unalignedNode(),org,dir,ray_near,ray_far,tNear);

          /*! process nodes with unaligned bounds and motion blur */
          else if (unlikely(cur.isUnalignedNodeMB(types)))
            mask = intersect_node(cur.unalignedNodeMB(),org,dir,ray_near,ray_far,ray.time,tNear);

          /*! if no child is hit, pop next node */
	  const BVH4::BaseNode* node = cur.baseNode(types);
          if (unlikely(mask == 0))
            goto pop;
	  
	  /*! one child is hit, continue with that child */
          size_t r = __bscf(mask);
          if (likely(mask == 0)) {
            cur = node->child(r); cur.prefetch(types); 
            assert(cur != BVH4::emptyNode);
            continue;
          }
          
          /*! two children are hit, push far child, and continue with closer child */
          NodeRef c0 = node->child(r); c0.prefetch(types); const unsigned int d0 = ((unsigned int*)&tNear)[r];
          r = __bscf(mask);
          NodeRef c1 = node->child(r); c1.prefetch(types); const unsigned int d1 = ((unsigned int*)&tNear)[r];
          assert(c0 != BVH4::emptyNode);
          assert(c1 != BVH4::emptyNode);
          if (likely(mask == 0)) {
            assert(stackPtr < stackEnd);
            if (d0 < d1) { *stackPtr = c1; stackPtr++; cur = c0; continue; }
            else         { *stackPtr = c0; stackPtr++; cur = c1; continue; }
          }
          assert(stackPtr < stackEnd);
          *stackPtr = c0; stackPtr++;
          assert(stackPtr < stackEnd);
          *stackPtr = c1; stackPtr++;
          
          /*! three children are hit */
          r = __bscf(mask);
          cur = node->child(r); cur.prefetch(types);
          assert(cur != BVH4::emptyNode);
          if (likely(mask == 0)) continue;
          assert(stackPtr < stackEnd);
          *stackPtr = cur; stackPtr++;
          
          /*! four children are hit */
          cur = node->child(3); cur.prefetch(types);
          assert(cur != BVH4::emptyNode);
        }
        
        /*! this is a leaf node */
	assert(cur != BVH4::emptyNode);
        STAT3(shadow.trav_leaves,1,1,1);
        size_t num; Primitive* prim = (Primitive*) cur.leaf(num);
        size_t lazy_node = 0;
        if (PrimitiveIntersector::occluded(pre,ray,prim,num,bvh->scene,lazy_node)) {
          ray.geomID = 0;
          break;
        }

        if (unlikely(lazy_node)) {
          *stackPtr = (NodeRef)lazy_node;
          stackPtr++;
        }
      }
      AVX_ZERO_UPPER();
    }

    DEFINE_INTERSECTOR1(BVH4Bezier1vIntersector1,BVH4Intersector1<0x1 COMMA false COMMA ArrayIntersector1<Bezier1vIntersector1> >);
    DEFINE_INTERSECTOR1(BVH4Bezier1iIntersector1,BVH4Intersector1<0x1 COMMA false COMMA ArrayIntersector1<Bezier1iIntersector1> >);
    
    DEFINE_INTERSECTOR1(BVH4Bezier1vIntersector1_OBB,BVH4Intersector1<0x101 COMMA false COMMA ArrayIntersector1<Bezier1vIntersector1> >);
    DEFINE_INTERSECTOR1(BVH4Bezier1iIntersector1_OBB,BVH4Intersector1<0x101 COMMA false COMMA ArrayIntersector1<Bezier1iIntersector1> >);
    DEFINE_INTERSECTOR1(BVH4Bezier1iMBIntersector1_OBB,BVH4Intersector1<0x1010 COMMA false COMMA ArrayIntersector1<Bezier1iIntersector1MB> >);
    DEFINE_INTERSECTOR1(BVH4Triangle4Intersector1Moeller,BVH4Intersector1<0x10001 COMMA false COMMA ArrayIntersector1<TriangleNIntersector1MoellerTrumbore<Triangle4 COMMA true> > >);
#if defined(__AVX__)
    DEFINE_INTERSECTOR1(BVH4Triangle8Intersector1Moeller,BVH4Intersector1<0x1 COMMA false COMMA ArrayIntersector1<TriangleNIntersector1MoellerTrumbore<Triangle8 COMMA true> > >);
#endif
    DEFINE_INTERSECTOR1(BVH4Triangle4vIntersector1Pluecker,BVH4Intersector1<0x1 COMMA true COMMA ArrayIntersector1<TriangleNvIntersector1Pluecker<Triangle4v COMMA true> > >);
    DEFINE_INTERSECTOR1(BVH4Triangle4iIntersector1Pluecker,BVH4Intersector1<0x1 COMMA true COMMA ArrayIntersector1<Triangle4iIntersector1Pluecker<true> > >);

    DEFINE_INTERSECTOR1(BVH4Subdivpatch1Intersector1,BVH4Intersector1<0x1 COMMA true COMMA ArrayIntersector1<SubdivPatch1Intersector1 > >);
    DEFINE_INTERSECTOR1(BVH4Subdivpatch1CachedIntersector1,BVH4Intersector1<0x1 COMMA true COMMA SubdivPatch1CachedIntersector1>);

    DEFINE_INTERSECTOR1(BVH4GridAOSIntersector1,BVH4Intersector1<0x1 COMMA true COMMA GridAOSIntersector1>);

    DEFINE_INTERSECTOR1(BVH4VirtualIntersector1,BVH4Intersector1<0x1 COMMA false COMMA ArrayIntersector1<ObjectIntersector1> >);

    DEFINE_INTERSECTOR1(BVH4Triangle4vMBIntersector1Moeller,BVH4Intersector1<0x10 COMMA false COMMA ArrayIntersector1<TriangleNMblurIntersector1MoellerTrumbore<Triangle4vMB COMMA true> > >);
    }
}

/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/DrawInstanced>
#include <osgEarth/CullingUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/Utils>
#include <osgEarth/Shaders>
#include <osgEarth/ObjectIndex>
#include <osgEarth/TextureBuffer>

#include <osg/ComputeBoundsVisitor>
#include <osgDB/ObjectWrapper>
#include <osgUtil/Optimizer>

#define LC "[DrawInstanced] "

using namespace osgEarth;
using namespace osgEarth::Util;

// Ref: http://sol.gfxile.net/instancing.html

//Uncomment to experiment with instance count adjustment
//#define USE_INSTANCE_LODS

//----------------------------------------------------------------------

namespace osgEarth { namespace Util
{
    class MakeTransformsStatic : public osg::NodeVisitor
    {
    public:
        MakeTransformsStatic() : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0);
        }

        void apply(osg::Transform& node)
        {
            node.setDataVariance(osg::Object::STATIC);
            traverse(node);
        }
    };
} }

namespace
{
#ifdef USE_INSTANCE_LODS

    struct LODCallback : public osg::Drawable::DrawCallback
    {
        LODCallback() : _first(true), _maxInstances(0) { }

        void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const
        {
            const osg::Geometry* geom = drawable->asGeometry();

            if ( _first && geom->getNumPrimitiveSets() > 0 )
            {
                _maxInstances = geom->getPrimitiveSet(0)->getNumInstances();
                _first = false;
            }

            const osg::BoundingBox bbox = geom->getBoundingBox();
            float radius = bbox.radius();

            osg::Vec3d centerView = bbox.center() * ri.getState()->getModelViewMatrix();
            float rangeToBS = (float)-centerView.z() - radius;

            // check for inherit mode (3.3.0+ only)
            osg::Camera* cam = ri.getCurrentCamera();

            // Problem: the camera stack is *always* size=1. So no access to the ref cam.
            if (cam->getReferenceFrame() == cam->ABSOLUTE_RF_INHERIT_VIEWPOINT &&
                ri.getCameraStack().size() > 1)
            {
                osg::Camera* refCam = *(ri.getCameraStack().end()-2);
                if ( refCam )
                {
                    osg::Vec3d centerWorld = centerView * cam->getInverseViewMatrix();
                    osg::Vec3d centerRefView = centerWorld * refCam->getViewMatrix();
                    rangeToBS = (float)(-centerRefView.z() - radius);
                }
            }

            // these should obviously be programmable
            const float maxDistance = 2000.0f;
            const float minDistance = 100.0f;

            float ratio = (rangeToBS-minDistance)/(maxDistance-minDistance);
            ratio = 1.0 - osg::clampBetween(ratio, 0.0f, 1.0f);
            // 1 = closest, 0 = farthest

            unsigned instances = (unsigned)(ratio*(float)_maxInstances);

            if ( instances > 0 )
            {
                for(unsigned i=0; i<geom->getNumPrimitiveSets(); ++i)
                {
                    const osg::PrimitiveSet* ps = geom->getPrimitiveSet(i);
                    const_cast<osg::PrimitiveSet*>(ps)->setNumInstances(instances);
                }

                drawable->drawImplementation(ri);
            }
        }

        mutable bool     _first;
        mutable unsigned _maxInstances;
    };
#endif // USE_INSTANCE_LODS

    struct ModelInstance
    {
        ModelInstance() : objectID( OSGEARTH_OBJECTID_EMPTY ) { }
        osg::Matrix matrix;
        ObjectID    objectID;
    };

    typedef std::map< osg::ref_ptr<osg::Node>, std::vector<ModelInstance> > ModelInstanceMap;

}

//----------------------------------------------------------------------


namespace
{
    struct StaticBBox : public osg::Drawable::ComputeBoundingBoxCallback
    {
        osg::BoundingBox _box;
        StaticBBox(const osg::BoundingBox& box) : _box(box) { }
        osg::BoundingBox computeBound(const osg::Drawable&) const { return _box; }
    };
}

using namespace DrawInstanced;

ConvertToDrawInstanced::ConvertToDrawInstanced(unsigned                numInstances,
                                               const osg::BoundingBox& bbox,
                                               bool                    optimize,
                                               osg::TextureBuffer*     tbo,
                                               int                     defaultUnit) :
_numInstances( numInstances ),
_bbox(bbox),
_optimize( optimize ),
_tbo(tbo),
_tboUnit(defaultUnit)
{
    setTraversalMode( TRAVERSE_ALL_CHILDREN );
    setNodeMaskOverride( ~0 );
    _bboxComputer = new StaticBBox(bbox);
}


void
ConvertToDrawInstanced::apply(osg::Drawable& drawable)
{
    osg::Geometry* geom = drawable.asGeometry();
    if ( geom )
    {
        if ( _optimize )
        {
            // activate VBOs
            geom->setUseDisplayList( false );
            geom->setUseVertexBufferObjects( true );
        }

        geom->setComputeBoundingBoxCallback(_bboxComputer.get());
        geom->dirtyBound();

        // convert to use DrawInstanced
        for( unsigned p=0; p<geom->getNumPrimitiveSets(); ++p )
        {
            osg::PrimitiveSet* ps = geom->getPrimitiveSet(p);
            ps->setNumInstances( _numInstances );
            _primitiveSets.push_back( ps );
        }

#ifdef USE_INSTANCE_LODS
        geom->setDrawCallback( new LODCallback() );
#endif
    }
    apply(static_cast<osg::Node&>(drawable));
}

void
ConvertToDrawInstanced::apply(osg::LOD& lod)
{
    // find the highest LOD:
    int   minIndex = 0;
    float minRange = FLT_MAX;
    for(unsigned i=0; i<lod.getNumRanges(); ++i)
    {
        if ( lod.getRangeList()[i].first < minRange )
        {
            minRange = lod.getRangeList()[i].first;
            minIndex = i;
        }
    }

    // remove all but the highest:
    osg::ref_ptr<osg::Node> highestLOD = lod.getChild( minIndex );
    lod.removeChildren( 0, lod.getNumChildren() );

    // add it back with a full range.
    lod.addChild( highestLOD.get(), 0.0f, FLT_MAX );

    apply(static_cast<osg::Group&>(lod));
}

void
ConvertToDrawInstanced::apply(osg::Node& node)
{
    osg::StateSet* stateSet = node.getStateSet();
    if (stateSet)
    {
        int numTexAttrs = stateSet->getNumTextureAttributeLists();
        _tboUnit = osg::maximum(_tboUnit, numTexAttrs);
    }
    traverse(node);
}

bool
DrawInstanced::install(osg::StateSet* stateset)
{
    if ( !stateset )
        return false;

    if ( !Registry::capabilities().supportsDrawInstanced() )
        return false;

    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
    vp->setName("DrawInstanced");
    osgEarth::Shaders pkg;
    pkg.load( vp, pkg.Instancing );

    return true;
}


void
DrawInstanced::remove(osg::StateSet* stateset)
{
    if ( !stateset )
        return;

    VirtualProgram* vp = VirtualProgram::get(stateset);
    if ( !vp )
        return;

    Shaders pkg;
    pkg.unload( vp, pkg.Instancing );
}


void InstanceGroup::traverse(osg::NodeVisitor& nv)
{
    if (nv.getTraversalMode() == osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
        for (unsigned int i = 0; i < getNumChildren(); ++i)
        {
            osg::Node* child = getChild(i);

            osg::ref_ptr< osg::MatrixTransform > mt = new osg::MatrixTransform;
            mt->addChild(child);

            for (unsigned int j = 0; j < _matrices.size(); ++j)
            {
                mt->setMatrix(_matrices[j]);
                mt->accept(nv);
            }
        }
    }
    else
    {
        osg::Group::traverse(nv);
    }
}

InstanceGroup::InstanceGroup()
{
}

InstanceGroup::InstanceGroup(const InstanceGroup& rhs, const osg::CopyOp& copyop):
    _matrices(rhs._matrices)
{
}

const std::vector< osg::Matrixf >& InstanceGroup::getMatrices() const
{
    return _matrices;
}

void InstanceGroup::setMatrices(const std::vector< osg::Matrixf >& values)
{
    _matrices = values;
}

void InstanceGroup::addMatrix(const osg::Matrixf& value)
{
    _matrices.push_back(value);
}



namespace osgEarth {
    namespace Serializers {
        namespace InstanceGroup
        {
            static bool checkMatrices(const osgEarth::Util::DrawInstanced::InstanceGroup& g)
            {
                return g.getMatrices().size() > 0;
            }

            static bool readMatrices(osgDB::InputStream& is, osgEarth::Util::DrawInstanced::InstanceGroup& g)
            {
                unsigned int size = is.readSize(); is >> is.BEGIN_BRACKET;
                for (unsigned int i = 0; i < size; ++i)
                {
                    osg::Matrixf value;
                    is >> value;
                    g.addMatrix(value);
                }
                is >> is.END_BRACKET;
                return true;
            }

            static bool writeMatrices(osgDB::OutputStream& os, const osgEarth::Util::DrawInstanced::InstanceGroup& g)
            {
                const std::vector< osg::Matrixf>& matrices = g.getMatrices();
                os.writeSize(matrices.size()); os << os.BEGIN_BRACKET << std::endl;
                for (auto itr = matrices.begin(); itr != matrices.end(); ++itr)
                {
                    os << *itr << std::endl;
                }
                os << os.END_BRACKET << std::endl;
                return true;
            }


            REGISTER_OBJECT_WRAPPER(
                InstanceGroup,
                new osgEarth::Util::DrawInstanced::InstanceGroup,
                osgEarth::Util::DrawInstanced::InstanceGroup,
                "osg::Object osg::Node osg::Group osgEarth::Util::DrawInstanced::InstanceGroup")
            {
                ADD_USER_SERIALIZER(Matrices);
            }
        }
    }
}


bool
DrawInstanced::convertGraphToUseDrawInstanced( osg::Group* parent )
{
    if ( !Registry::capabilities().supportsDrawInstanced() )
        return false;

    // place a static bounding sphere on the graph since we intend to alter
    // the structure of the subgraph.
    const osg::BoundingSphere& bs = parent->getBound();
    parent->setInitialBound(bs);
    //parent->setComputeBoundingSphereCallback(new StaticBound(bs));
    parent->setCullingActive(false);
    parent->dirtyBound();

    ModelInstanceMap models;

    // collect the matrices for all the MT's under the parent. Obviously this assumes
    // a particular scene graph structure.
    for( unsigned i=0; i < parent->getNumChildren(); ++i )
    {
        // each MT in the group parents the same child.
        osg::MatrixTransform* mt = dynamic_cast<osg::MatrixTransform*>( parent->getChild(i) );
        if ( mt )
        {
            osg::Node* n = mt->getChild(0);
            //models[n].push_back( mt->getMatrix() );

            ModelInstance instance;
            instance.matrix = mt->getMatrix();

            // See whether the ObjectID is encoded in a uniform on the MT.
            osg::StateSet* stateSet = mt->getStateSet();
            if ( stateSet )
            {
                osg::Uniform* uniform = stateSet->getUniform( Registry::objectIndex()->getObjectIDUniformName() );
                if ( uniform )
                {
                    uniform->get( (unsigned&)instance.objectID );
                }
            }

            models[n].push_back( instance );
        }
    }

    // get rid of the old matrix transforms.
    parent->removeChildren(0, parent->getNumChildren());

	// This is the maximum size of the tbo
	int maxTBOSize = Registry::capabilities().getMaxTextureBufferSize();
	// This is the total number of instances it can store
	// We will iterate below. If the number of instances is larger than the buffer can store
    // we make more tbos
    int matrixSize = 4 * 4 * sizeof(float); // 4 vec4's.
    int maxTBOInstancesSize = maxTBOSize / matrixSize;

    osgUtil::Optimizer optimizer;

    // For each model:
    for( ModelInstanceMap::iterator i = models.begin(); i != models.end(); ++i )
    {
        osg::Node*                  node      = i->first.get();
        std::vector<ModelInstance>& instances = i->second;

        // calculate the overall bounding box for the model:
        osg::ComputeBoundsVisitor cbv;
        node->accept( cbv );
        const osg::BoundingBox& nodeBox = cbv.getBoundingBox();

        osg::BoundingBox bbox;
        for( std::vector<ModelInstance>::iterator m = instances.begin(); m != instances.end(); ++m )
        {
            bbox.expandBy(nodeBox.corner(0) * m->matrix);
            bbox.expandBy(nodeBox.corner(1) * m->matrix);
            bbox.expandBy(nodeBox.corner(2) * m->matrix);
            bbox.expandBy(nodeBox.corner(3) * m->matrix);
            bbox.expandBy(nodeBox.corner(4) * m->matrix);
            bbox.expandBy(nodeBox.corner(5) * m->matrix);
            bbox.expandBy(nodeBox.corner(6) * m->matrix);
            bbox.expandBy(nodeBox.corner(7) * m->matrix);
        }

		unsigned tboSize = 0;
		unsigned numInstancesToStore = 0;

		if (instances.size()<maxTBOInstancesSize)
		{
			tboSize = instances.size();
			numInstancesToStore = instances.size();
		}
		else
		{
			OE_WARN << "Number of Instances: " << instances.size() << " exceeds Number of instances TBO can store: " << maxTBOInstancesSize << std::endl;
			OE_WARN << "Storing maximum possible instances in TBO, and skipping the rest"<<std::endl;
			tboSize = maxTBOInstancesSize;
			numInstancesToStore = maxTBOInstancesSize;
		}

        // this group is simply a container for the uniform:
        InstanceGroup* instanceGroup = new InstanceGroup();

        // sampler that will hold the instance matrices:
        osg::Image* image = new osg::Image();
        image->setName("osgearth.drawinstanced.postex");
		image->allocateImage( tboSize*4, 1, 1, GL_RGBA, GL_FLOAT );

		// could use PixelWriter but we know the format.
		// Note: we are building a transposed matrix because it makes the decoding easier in the shader.
		GLfloat* ptr = reinterpret_cast<GLfloat*>( image->data() );
		for(unsigned m=0; m<numInstancesToStore; ++m)
		{
			ModelInstance& i = instances[m];
			const osg::Matrixf& mat = i.matrix;

			// copy the first 3 columns:
			for(int col=0; col<3; ++col)
			{
				for(int row=0; row<4; ++row)
				{
					*ptr++ = mat(row,col);
				}
			}

			// encode the ObjectID in the last column, which is always (0,0,0,1)
			// in a standard scale/rot/trans matrix. We will reinstate it in the
			// shader after extracting the object ID.
			*ptr++ = (float)((i.objectID      ) & 0xff);
			*ptr++ = (float)((i.objectID >>  8) & 0xff);
			*ptr++ = (float)((i.objectID >> 16) & 0xff);
			*ptr++ = (float)((i.objectID >> 24) & 0xff);

			// store them int the metadata as well
            instanceGroup->addMatrix(mat);
		}

        // so the TBO will serialize properly.
        image->setWriteHint(osg::Image::STORE_INLINE);

        // Constuct the TBO:
        osg::TextureBuffer* posTBO = new osgEarth::TextureBuffer;
		posTBO->setImage(image);
        posTBO->setInternalFormat( GL_RGBA32F_ARB );
        posTBO->setUnRefImageDataAfterApply( false );

        // Flatten any transforms in the node graph:
        MakeTransformsStatic makeStatic;
        node->accept(makeStatic);
        osgUtil::Optimizer::FlattenStaticTransformsDuplicatingSharedSubgraphsVisitor flatten;
        node->accept(flatten);

        // Convert the node's primitive sets to use "draw-instanced" rendering; at the
        // same time, assign our computed bounding box as the static bounds for all
        // geometries. (As DI's they cannot report bounds naturally.)
        ConvertToDrawInstanced cdi(numInstancesToStore, bbox, true, posTBO, 0);
        node->accept( cdi );

        // Bind the TBO sampler:
        osg::StateSet* stateset = instanceGroup->getOrCreateStateSet();
        stateset->setTextureAttribute(cdi.getTextureImageUnit(), posTBO);
        stateset->getOrCreateUniform("oe_di_postex_TBO", osg::Uniform::SAMPLER_BUFFER)->set(cdi.getTextureImageUnit());

        // Tell the SG to skip the positioning TBO.
        ShaderGenerator::setIgnoreHint(posTBO, true);

		// add the node as a child:
        instanceGroup->addChild( node );

        parent->addChild( instanceGroup );

        //OE_INFO << LC << "ConvertToDI: instances=" << numInstancesToStore << "\n";
    }

    return true;
}
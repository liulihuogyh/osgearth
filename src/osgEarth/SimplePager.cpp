#include <osgEarth/SimplePager> 
#include <osgEarth/TileKey>
#include <osgEarth/Utils>
#include <osgEarth/CullingUtils>
#include <osgEarth/Metrics>
#include <osgEarth/PagedNode>
#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <osg/ShapeDrawable>
#include <osg/MatrixTransform>

using namespace osgEarth::Util;

#define LC "[SimplerPager] "

#define USE_PAGING_MANAGER

namespace
{
    /**
     * The master progress tracker keeps track of the current framestamp
     * for the entire paged scene graph.
     */
    struct ProgressMaster : public osg::NodeCallback
    {
        unsigned _frame;
        bool _canCancel;
        bool _done;

        ProgressMaster() : _canCancel(true), _done(false) { }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            _frame = nv->getFrameStamp() ? nv->getFrameStamp()->getFrameNumber() : 0u;
            traverse(node, nv);
        }
    };

    /**
     * The ProgressCallback that gets passed to createNode() for the subclass
     * to use. It will report cancelation if the last reported frame number is
     * behind the current master frame number (as tracked by the ProgressMaster)
     */
    struct MyProgressCallback : public DatabasePagerProgressCallback
    {
        MyProgressCallback(ProgressMaster* master) :
            DatabasePagerProgressCallback()
        {
            _master = master;
            _lastFrame = 0u;
        }

        virtual bool shouldCancel() const
        {
            osg::ref_ptr<ProgressMaster> master(_master.get());

            bool should = 
                (DatabasePagerProgressCallback::shouldCancel()) ||
                (master.valid() == false) ||
                (master->_done == true) ||
                (master->_canCancel && _master->_frame - _lastFrame > 1u);

            if (should)
            {
                OE_DEBUG << LC << "Canceling SP task on thread " << std::this_thread::get_id() << std::endl;
            }

            return should;
        }

        // called by ProgressUpdater
        void touch(const osg::FrameStamp* stamp)
        {
            if ( stamp )
                _lastFrame = stamp->getFrameNumber();
        }

        unsigned _lastFrame;
        osg::observer_ptr<ProgressMaster> _master;
    };


#ifndef USE_PAGING_MANAGER
    /**
    * A pseudo-loader for paged feature tiles.
    */
    struct SimplePagerPseudoLoader : public osgDB::ReaderWriter
    {
        SimplePagerPseudoLoader()
        {
            supportsExtension( "osgearth_pseudo_simple", "" );
        }

        const char* className() const
        { // override
            return "Simple Pager";
        }

        ReadResult readNode(const std::string& uri, const Options* options) const
        {
            if ( !acceptsExtension( osgDB::getLowerCaseFileExtension(uri) ) )
                return ReadResult::FILE_NOT_HANDLED;

            unsigned int lod, x, y;
            sscanf( uri.c_str(), "%u_%u_%u.%*s", &lod, &x, &y );

            osg::ref_ptr<SimplePager> pager;
            if (!ObjectStorage::get(options, pager))
            {
                OE_WARN << LC << "Internal error - no SimplePager object in ObjectStorage\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }

            osg::ref_ptr<SimplePager::ProgressTracker> tracker;
            if (!ObjectStorage::get(options, tracker))
            {
                OE_WARN << LC << "Internal error - no ProgressTracker object in ObjectStorage\n";
                return ReadResult::ERROR_IN_READING_FILE;
            }
            
            OE_SCOPED_THREAD_NAME("DBPager", getName());

            osg::ref_ptr<osg::Node> node = pager->loadKey(
                TileKey(lod, x, y, pager->getProfile()),
                tracker.get());

            if (node.valid() && pager->getSceneGraphCallbacks())
            {
                pager->getSceneGraphCallbacks()->firePreMergeNode(node.get());
            }

            return ReadResult(node);
        }
    };

    REGISTER_OSGPLUGIN(osgearth_pseudo_simple, SimplePagerPseudoLoader);
#endif
}


SimplePager::ProgressTracker::ProgressTracker(osg::NodeCallback* master)
{
    setName( "osgEarth::Util::SimplePager::ProgressTracker" );
    for(int i=0; i<4; ++i)
        _progress[i] = new MyProgressCallback( static_cast<ProgressMaster*>(master) );
}

void SimplePager::ProgressTracker::operator()(osg::Node* node, osg::NodeVisitor* nv)
{
    for(int i=0; i<4; ++i)
        static_cast<MyProgressCallback*>(_progress[i].get())->touch( nv->getFrameStamp() );
    traverse(node, nv);
}


SimplePager::SimplePager(const osgEarth::Profile* profile):
_profile( profile ),
_rangeFactor( 6.0 ),
_additive(false),
_minLevel(0),
_maxLevel(30),
_priorityScale(1.0f),
_priorityOffset(0.0f),
_canCancel(true),
_mutex("SimplePager(OE)")
{
    // required in order to pass our "this" pointer to the pseudo loader:
    this->setName("osgEarth::Util::SimplePager::this" );
    
    // install the master framestamp tracker:
    _progressMaster = new ProgressMaster();
    addCullCallback( _progressMaster.get() );
}

void SimplePager::setEnableCancelation(bool value)
{
    static_cast<ProgressMaster*>(_progressMaster.get())->_canCancel = value;
}

bool SimplePager::getEnableCancalation() const
{
    return static_cast<ProgressMaster*>(_progressMaster.get())->_canCancel;
}

void SimplePager::build()
{
    addChild( buildRootNode() );
}

void SimplePager::shutdown()
{
    static_cast<ProgressMaster*>(_progressMaster.get())->_done = true;
}

osg::BoundingSphere SimplePager::getBounds(const TileKey& key) const
{
    int samples = 6;

    GeoExtent extent = key.getExtent();

    double xSample = extent.width() / (double)samples;
    double ySample = extent.height() / (double)samples;

    osg::BoundingSphere bs;
    for (int c = 0; c < samples+1; c++)
    {
        double x = extent.xMin() + (double)c * xSample;
        for (int r = 0; r < samples+1; r++)
        {
            double y = extent.yMin() + (double)r * ySample;
            osg::Vec3d world;

            GeoPoint samplePoint(extent.getSRS(), x, y, 0, ALTMODE_ABSOLUTE);

            GeoPoint wgs84 = samplePoint.transform(osgEarth::SpatialReference::create("epsg:4326"));
            wgs84.toWorld(world);
            bs.expandBy(world);
        }
    }
    return bs;
}

osg::ref_ptr<osg::Node> SimplePager::buildRootNode()
{   
#ifdef USE_PAGING_MANAGER
    osg::ref_ptr<osg::Group> root = new PagingManager();
#else
    osg::ref_ptr<osg::Group> root = new osg::Group();
#endif

    std::vector<TileKey> keys;
    _profile->getRootKeys( keys );
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        osg::ref_ptr<osg::Node> node = createPagedNode( keys[i], 0L );
        if ( node.valid() )
            root->addChild( node );
    }

    return root;
}

osg::ref_ptr<osg::Node> SimplePager::createNode(const TileKey& key, ProgressCallback* progress)
{
    osg::BoundingSphere bounds = getBounds( key );

    osg::MatrixTransform* mt = new osg::MatrixTransform;
    mt->setMatrix(osg::Matrixd::translate( bounds.center() ) );
    osg::Geode* geode = new osg::Geode;
    osg::ShapeDrawable* sd = new osg::ShapeDrawable( new osg::Sphere(osg::Vec3f(0,0,0), bounds.radius()) );
    sd->setColor( osg::Vec4(1,0,0,1 ) );
    geode->addDrawable( sd );
    mt->addChild(geode);
    return mt;
}

#ifdef USE_PAGING_MANAGER

osg::ref_ptr<osg::Node>
SimplePager::createPagedNode(const TileKey& key, ProgressCallback* progress)
{
    osg::BoundingSphere tileBounds = getBounds(key);
    float tileRadius = tileBounds.radius();

    // restrict subdivision to max level:
    bool hasChildren = key.getLOD() < _maxLevel;

    // Create the actual data for this tile.
    osg::ref_ptr<osg::Node> node;

    // only create real node if we are at least at the min LOD:
    if (key.getLOD() >= _minLevel)
    {
        node = createNode(key, progress);
        hasChildren = node.valid();
    }

    osg::ref_ptr<PagedNode2> pagedNode = new PagedNode2();
    pagedNode->setSceneGraphCallbacks(getSceneGraphCallbacks());

    if (node.valid())
    {
        pagedNode->addChild(node);
        fire_onCreateNode(key, node.get());
    }

    tileRadius = osg::maximum(
        tileBounds.radius(), 
        static_cast<osg::BoundingSphere::value_type>(tileRadius));

    pagedNode->setCenter(tileBounds.center());
    pagedNode->setRadius(tileRadius);

    // Assume geocentric for now.
    if (true)
    {
        const GeoExtent& ccExtent = key.getExtent();
        if (ccExtent.isValid())
        {
            // if the extent is more than 90 degrees, bail
            GeoExtent geodeticExtent = ccExtent.transform(ccExtent.getSRS()->getGeographicSRS());
            if (geodeticExtent.width() < 90.0 && geodeticExtent.height() < 90.0)
            {
                // get the geocentric tile center:
                osg::Vec3d tileCenter;
                ccExtent.getCentroid(tileCenter.x(), tileCenter.y());

                osg::Vec3d centerECEF;
                const SpatialReference* mapSRS = osgEarth::SpatialReference::get("epsg:4326");
                if (mapSRS)
                {
                    ccExtent.getSRS()->transform(tileCenter, mapSRS->getGeocentricSRS(), centerECEF);
                    osg::NodeCallback* ccc = ClusterCullingFactory::create(geodeticExtent);
                    if (ccc)
                        pagedNode->addCullCallback(ccc);
                }
            }
        }
    }

    float loadRange = FLT_MAX;

    if (hasChildren)
    {
        if (getName().empty())
            pagedNode->setName(key.str());
        else
            pagedNode->setName(getName() + " " + key.str());

        // install a callback that will update the progress tracker whenever the PagedNode
        // gets traversed. The children, once activated, will have access to its
        // ProgressCallback within and be able to check for cancelation, use stats, etc.
        //ProgressTracker* tracker = new ProgressTracker(_progressMaster.get());
        //pagedNode->addCullCallback(tracker);

        // Now setup a filename on the PagedLOD that will load all of the children of this node.
        pagedNode->setPriorityScale(_priorityScale);
        //pager->setPriorityOffset(_priorityOffset);

        pagedNode->setLoadFunction(
            [this, key](Cancelable* progress)
            {
                return loadKey(key, nullptr); // tracker);
            }
        );

        loadRange = (float)(tileRadius * _rangeFactor);
        pagedNode->setRefinePolicy(_additive ? pagedNode->REFINE_ADD : pagedNode->REFINE_REPLACE);
    }

    pagedNode->setMaxRange(loadRange);

    //OE_INFO << "PagedNode2: key="<<key.str()<<" hasChildren=" << hasChildren << ", range=" << loadRange << std::endl;

    return pagedNode;
}

#else

osg::ref_ptr<osg::Node> SimplePager::createPagedNode(const TileKey& key, ProgressCallback* progress)
{
    osg::BoundingSphere tileBounds = getBounds( key );
    float tileRadius = tileBounds.radius();

    // restrict subdivision to max level:
    bool hasChildren = key.getLOD() < _maxLevel;

    // Create the actual data for this tile.
    osg::ref_ptr<osg::Node> node;

    // only create real node if we are at least at the min LOD:
    if ( key.getLevelOfDetail() >= _minLevel )
    {
        node = createNode( key, progress );

        if ( node.valid())
        {      
            // TODO:  Some bounds can be invalid, specifically from PlaceNodes, so we just take the computed tile bounds instead.
            // tileBounds = node->getBound();         
        }
        else
        {
            hasChildren = false;
        }
    }

    if ( !node.valid() )
    {
        node = new osg::Group();
    }

    // notify any callbacks.
    fire_onCreateNode(key, node.get());

    tileRadius = osg::maximum(tileBounds.radius(), static_cast<osg::BoundingSphere::value_type>(tileRadius));

    osg::ref_ptr<osg::PagedLOD> plod = 
        getSceneGraphCallbacks() ? new PagedLODWithSceneGraphCallbacks(getSceneGraphCallbacks()) :
        new osg::PagedLOD();

    plod->setCenter( tileBounds.center() ); 
    plod->setRadius( tileRadius );

    plod->addChild( node.get() );
    
    // Assume geocentric for now.
    if (true)
    {
        const GeoExtent& ccExtent = key.getExtent();
        if (ccExtent.isValid())
        {
            // if the extent is more than 90 degrees, bail
            GeoExtent geodeticExtent = ccExtent.transform(ccExtent.getSRS()->getGeographicSRS());
            if (geodeticExtent.width() < 90.0 && geodeticExtent.height() < 90.0)
            {
                // get the geocentric tile center:
                osg::Vec3d tileCenter;
                ccExtent.getCentroid(tileCenter.x(), tileCenter.y());

                osg::Vec3d centerECEF;
                const SpatialReference* mapSRS = osgEarth::SpatialReference::get("epsg:4326");
                if (mapSRS)
                {
                    ccExtent.getSRS()->transform(tileCenter, mapSRS->getGeocentricSRS(), centerECEF);
                    osg::NodeCallback* ccc = ClusterCullingFactory::create(geodeticExtent);
                    if (ccc)
                        plod->addCullCallback(ccc);
                }
            }
        }
    }

    if ( hasChildren )
    {
        std::stringstream buf;
        buf << key.getLevelOfDetail() << "_" << key.getTileX() << "_" << key.getTileY() << ".osgearth_pseudo_simple";

        std::string uri = buf.str();

        // Now setup a filename on the PagedLOD that will load all of the children of this node.
        plod->setFileName(1, uri);
        plod->setPriorityOffset(1, _priorityOffset);
        plod->setPriorityScale (1, _priorityScale);
        
        // install a callback that will update the progress tracker whenever the PLOD
        // gets traversed. The children, once activated, will have access to its
        // ProgressCallback within and be able to check for cancelation, use stats, etc.
        ProgressTracker* tracker = new ProgressTracker( _progressMaster.get() );
        plod->addCullCallback( tracker );

        // assemble data to pass to the pseudoloader
        osgDB::Options* options = new osgDB::Options();
        ObjectStorage::set(options, this);
        ObjectStorage::set(options, tracker);
        plod->setDatabaseOptions( options );
        
        // Install an FLC if the caller provided one
        if ( _fileLocationCallback.valid() )
            options->setFileLocationCallback( _fileLocationCallback.get() );

        // Setup the min and max ranges.
        float minRange = (float)(tileRadius * _rangeFactor);

        if (!_additive)
        {
            // Replace mode, the parent is replaced by its children.
            plod->setRange( 0, minRange, FLT_MAX );
            plod->setRange( 1, 0, minRange );
        }
        else
        {
            // Additive, the parent remains and new data is added
            plod->setRange( 0, 0, FLT_MAX );
            plod->setRange( 1, 0, minRange );
        }
    }
    else
    {
        // no children, so max out the visibility range.
        plod->setRange( 0, 0, FLT_MAX );
    }

    return plod;
}
#endif

/**
* Loads the PagedLOD hierarchy for this key.
*/
osg::ref_ptr<osg::Node>
SimplePager::loadKey(const TileKey& key, ProgressTracker* tracker)
{       
    osg::ref_ptr< osg::Group >  group = new osg::Group;

    for (unsigned int i = 0; i < 4; i++)
    {
        TileKey childKey = key.createChildKey( i );

        osg::ref_ptr<osg::Node> plod = createPagedNode(childKey, nullptr); // tracker->_progress[i].get() );
        if (plod.valid())
        {
            group->addChild( plod );
        }
    }
    if (group->getNumChildren() > 0)
    {
        return group;
    }
    return nullptr;
}

const osgEarth::Profile* SimplePager::getProfile() const
{
    return _profile.get();
}

void SimplePager::addCallback(Callback* callback)
{
    if (callback)
    {
        Threading::ScopedMutexLock lock(_mutex);
        _callbacks.push_back(callback);
    }
}

void SimplePager::removeCallback(Callback* callback)
{
    if (callback)
    {
        Threading::ScopedMutexLock lock(_mutex);
        for (Callbacks::iterator i = _callbacks.begin(); i != _callbacks.end(); ++i)
        {
            if (i->get() == callback)
            {
                _callbacks.erase(i);
                break;
            }
        }
    }
}

void SimplePager::fire_onCreateNode(const TileKey& key, osg::Node* node)
{
    Threading::ScopedMutexLock lock(_mutex);
    for (Callbacks::iterator i = _callbacks.begin(); i != _callbacks.end(); ++i)
        i->get()->onCreateNode(key, node);
}
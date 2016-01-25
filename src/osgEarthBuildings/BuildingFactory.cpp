/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2016 Pelican Mapping
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
#include "BuildingFactory"
#include "BuildingSymbol"
#include "BuildingVisitor"
#include "BuildContext"
#include "Parapet"
#include <osgEarthFeatures/AltitudeFilter>
#include <osgEarthSymbology/Geometry>

using namespace osgEarth;
using namespace osgEarth::Buildings;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

#define LC "[BuildingFactory] "

BuildingFactory::BuildingFactory()
{
    _session = new Session(0L);
}

void
BuildingFactory::setSession(Session* session)
{
    _session = session;
    if ( session )
    {
        _eq.setMapFrame( session->createMapFrame() );
        _eq.setFallBackOnNoData( true );
    }
}

bool
BuildingFactory::cropToCentroid(const Feature* feature, const GeoExtent& extent) const
{
    if ( !extent.isValid() )
        return true;

    // make sure the centroid is in the crop-to extent: 
    GeoPoint centroid( feature->getSRS(), feature->getGeometry()->getBounds().center() );
    return extent.contains(centroid);
}

namespace
{
    struct BuildingClamper : public BuildingVisitor
    {
        float _min, _max;
        BuildingClamper(float min, float max) : _min(min), _max(max) { }

        void apply(Elevation* elev)
        {
            Elevation::Walls& walls = elev->getWalls();
            for(Elevation::Walls::iterator w = walls.begin(); w != walls.end(); ++w)
            {
                for(Elevation::Faces::iterator f = w->faces.begin(); f != w->faces.end(); ++f)
                {
                    f->left.lower.z()  += _min;
                    f->left.upper.z()  += _min;
                    f->right.lower.z() += _min;
                    f->right.upper.z() += _min;
#if 0
                    if ( elev->isBasement() )
                    {
                        f->left.lower.z()  = _min;
                        f->left.upper.z()  = _max;
                        f->right.lower.z() = _min;
                        f->right.upper.z() = _max;
                    
                        f->left.height  = _max - _min;
                        f->right.height = _max - _min;
                    }
                    else
                    {
                        f->left.lower.z()  += _max;
                        f->left.upper.z()  += _max;
                        f->right.lower.z() += _max;
                        f->right.upper.z() += _max;
                    }
#endif
                }
            }
            traverse(elev);
        }
    };
}

void
BuildingFactory::calculateTerrainMinMax(Feature* feature, float& min, float& max)
{
    if ( !feature || !feature->getGeometry() )
        return;

    float maxRes = 0.0f;

    GeometryIterator gi(feature->getGeometry(), false);
    while(gi.hasMore())
    {
        Geometry* part = gi.next();
        std::vector<double> elevations;
        elevations.reserve( part->size() );
        if ( _eq.getElevations(part->asVector(), feature->getSRS(), elevations, maxRes) )
        {
            for(unsigned i=0; i<elevations.size(); ++i)
            {
                float e = elevations[i];
                if ( e < min ) min = e;
                if ( e > max ) max = e;
            }
        }
    }
}

bool
BuildingFactory::create(FeatureCursor*    input,
                        const GeoExtent&  cropTo,
                        const Style*      style,
                        BuildingVector&   output,
                        ProgressCallback* progress)
{
    if ( !input )
        return false;

    bool needToClamp = 
        style &&
        style->has<AltitudeSymbol>() &&
        style->get<AltitudeSymbol>()->clamping() != AltitudeSymbol::CLAMP_NONE;

    // iterate over all the input features
    while( input->hasMore() )
    {
        // for each feature, check that it's a polygon
        Feature* feature = input->nextFeature();
        if ( feature && feature->getGeometry() )
        {
            // Removing co-linear points will help produce a more "true"
            // longest-edge for rotation and roof rectangle calcuations.
            feature->getGeometry()->removeColinearPoints();

            if ( _outSRS.valid() )
            {
                feature->transform( _outSRS.get() );
            }

            // this ensures that the feature's centroid is in our bounding
            // extent, so that a feature doesn't end up in multiple extents
            if ( !cropToCentroid(feature, cropTo) )
            {
                continue;
            }


            // clamp to the terrain.
            float min = FLT_MAX, max = -FLT_MAX;
            if ( needToClamp )
            {
                calculateTerrainMinMax(feature, min, max);
            }
            bool terrainMinMaxValid = (min < max);

            unsigned offset = output.size();

            if ( _catalog.valid() )
            {
                float minHeight = terrainMinMaxValid ? max-min+3.0f : 3.0f;
                _catalog->createBuildings(feature, _session.get(), style, minHeight, output, progress);
            }

            else
            {
                Building* building = createBuilding(feature, progress);
                if ( building )
                {
                    output.push_back( building );
                }
            }

            if ( min < FLT_MAX && max > -FLT_MAX )
            {
                BuildingClamper clamper(min, max);
                for(unsigned i=offset; i<output.size(); ++i)
                {
                    output[i]->accept( clamper );
                }
            }
        }
    }

    return true;
}

Building*
BuildingFactory::createBuilding(Feature* feature, ProgressCallback* progress)
{
    if ( feature == 0L )
        return 0L;

    osg::ref_ptr<Building> building;

    Geometry* geometry = feature->getGeometry();

    if ( geometry && geometry->getComponentType() == Geometry::TYPE_POLYGON && geometry->isValid() )
    {
        // Calculate a local reference frame for this building:
        osg::Vec2d center2d = geometry->getBounds().center2d();
        GeoPoint centerPoint( feature->getSRS(), center2d.x(), center2d.y(), 0.0, ALTMODE_ABSOLUTE );

        osg::Matrix local2world, world2local;
        centerPoint.createLocalToWorld( local2world );
        world2local.invert( local2world );

        // Transform feature geometry into the local frame. This way we can do all our
        // building creation in cartesian, single-precision space.
        GeometryIterator iter(geometry, true);
        while(iter.hasMore())
        {
            Geometry* part = iter.next();
            for(Geometry::iterator i = part->begin(); i != part->end(); ++i)
            {
                osg::Vec3d world;
                feature->getSRS()->transformToWorld( *i, world );
                (*i) = world * world2local;
            }
        }

        BuildContext context;
        context.setSeed( feature->getFID() );

        // Next, iterate over the polygons and set up the Building object.
        GeometryIterator iter2( geometry, false );
        while(iter2.hasMore())
        {
            Polygon* polygon = dynamic_cast<Polygon*>(iter2.next());
            if ( polygon && polygon->isValid() )
            {
                // A footprint is the minumum info required to make a building.
                building = createSampleBuilding( feature );

                // Install the reference frame of the footprint geometry:
                building->setReferenceFrame( local2world );

                // Do initial cleaning of the footprint and install is:
                cleanPolygon( polygon );

                // Finally, build the internal structure from the footprint.
                building->build( polygon, context );
            }
            else
            {
                OE_WARN << LC << "Feature " << feature->getFID() << " is not a polygon. Skipping..\n";
            }
        }
    }

    return building.release();
}

void
BuildingFactory::cleanPolygon(Polygon* polygon)
{
    polygon->open();

    polygon->removeDuplicates();

    polygon->rewind( Polygon::ORIENTATION_CCW );

    // TODO: remove colinear points? for skeleton?
}

Building*
BuildingFactory::createSampleBuilding(const Feature* feature)
{
    Building* building = new Building();
    building->setUID( feature->getFID() );

    // figure out the building's height and number of floors.
    // single-elevation building.
    float height       = 15.0f;
    unsigned numFloors = 1u;

    // Add a single elevation.
    Elevation* elevation = new Elevation();
    building->getElevations().push_back(elevation);
    
    Roof* roof = new Roof();
    roof->setType( Roof::TYPE_FLAT );
    elevation->setRoof( roof );
    
    SkinResource* wallSkin = 0L;
    SkinResource* roofSkin = 0L;

    if ( _session.valid() )
    {
        ResourceLibrary* reslib = _session->styles()->getDefaultResourceLibrary();
        if ( reslib )
        {
            wallSkin = reslib->getSkin( "facade.commercial.1" );
            elevation->setSkinResource( wallSkin );

            roofSkin = reslib->getSkin( "roof.commercial.1" );
            roof->setSkinResource( roofSkin );
        }
        else
        {
            //OE_WARN << LC << "No resource library\n";
        }

        const BuildingSymbol* sym = _session->styles()->getDefaultStyle()->get<BuildingSymbol>();
        if ( sym )
        {
            if ( feature )
            {
                NumericExpression heightExpr = sym->height().get();
                height = feature->eval( heightExpr, _session.get() );
            }

            // calculate the number of floors
            if ( wallSkin )
            {
                numFloors = (unsigned)std::max(1.0f, osg::round(height / wallSkin->imageHeight().get()));
            }
            else
            {
                numFloors = (unsigned)std::max(1.0f, osg::round(height / sym->floorHeight().get()));
            }
        }
    }

    elevation->setHeight( height );
    elevation->setNumFloors( numFloors );

    Parapet* parapet = new Parapet();
    parapet->setParent( elevation );
    parapet->setWidth( 2.0f );
    parapet->setHeight( 2.0f );
    parapet->setNumFloors( 1u );

    parapet->setColor( Color::Gray.brightness(1.3f) );
    parapet->setRoof( new Roof() );
    parapet->getRoof()->setSkinResource( roofSkin );
    parapet->getRoof()->setColor( Color::Gray.brightness(1.2f) );

    elevation->getElevations().push_back( parapet );

    return building;
}

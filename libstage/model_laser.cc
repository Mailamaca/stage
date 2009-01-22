///////////////////////////////////////////////////////////////////////////
//
// File: model_laser.c
// Author: Richard Vaughan
// Date: 10 June 2004
//
// CVS info:
//  $Source: /home/tcollett/stagecvs/playerstage-cvs/code/stage/libstage/model_laser.cc,v $
//  $Author: rtv $
//  $Revision: 1.7 $
//
///////////////////////////////////////////////////////////////////////////

//#define DEBUG 

#include <sys/time.h>

#include "stage.hh"
#include "worldfile.hh"
#include "option.hh"
using namespace Stg;

// DEFAULT PARAMETERS FOR LASER MODEL
static const bool DEFAULT_FILLED = true;
static const stg_watts_t DEFAULT_WATTS = 17.5;
static const Size DEFAULT_SIZE( 0.15, 0.15, 0.2 );
static const stg_meters_t DEFAULT_MINRANGE = 0.0;
static const stg_meters_t DEFAULT_MAXRANGE = 8.0;
static const stg_radians_t DEFAULT_FOV = M_PI;
static const unsigned int DEFAULT_SAMPLES = 180;
static const stg_msec_t DEFAULT_INTERVAL_MS = 100;
static const unsigned int DEFAULT_RESOLUTION = 1;
static const char* DEFAULT_COLOR = "blue";

//TODO make instance attempt to register an option (as customvisualizations do)
Option ModelLaser::showLaserData( "Laser scans", "show_laser", "", true, NULL );
Option ModelLaser::showLaserStrikes( "Laser strikes", "show_laser_strikes", "", false, NULL );

/**
@ingroup model
@defgroup model_laser Laser model 
The laser model simulates a scanning laser rangefinder

API: Stg::ModelLaser

<h2>Worldfile properties</h2>

@par Summary and default values

@verbatim
laser
(
  # laser properties
  samples 180
  range_min 0.0
  range_max 8.0
  fov 3.14159
  resolution 1

  # model properties
  size [ 0.15 0.15 0.2 ]
  color "blue"
)
@endverbatim

@par Details
 
- samples <int>\n
  the number of laser samples per scan
- range_min <float>\n
  the minimum range reported by the scanner, in meters. The scanner will detect objects closer than this, but report their range as the minimum.
- range_max <float>\n
  the maximum range reported by the scanner, in meters. The scanner will not detect objects beyond this range.
- fov <float>\n
  the angular field of view of the scanner, in radians. 
- resolution <int>\n
  Only calculate the true range of every nth laser sample. The missing samples are filled in with a linear interpolation. Generally it would be better to use fewer samples, but some (poorly implemented!) programs expect a fixed number of samples. Setting this number > 1 allows you to reduce the amount of computation required for your fixed-size laser vector.
*/
  
  ModelLaser::ModelLaser( World* world, 
										  Model* parent )
  : Model( world, parent, MODEL_TYPE_LASER ),
	data_dl(0),
	data_dirty( true ),
	samples( NULL ),	// don't allocate sample buffer memory until Update() is called
	sample_count( DEFAULT_SAMPLES ),
	range_min( DEFAULT_MINRANGE ),
	range_max( DEFAULT_MAXRANGE ),
	fov( DEFAULT_FOV ),
	resolution( DEFAULT_RESOLUTION )
{
  
  PRINT_DEBUG2( "Constructing ModelLaser %d (%s)\n", 
					 id, typestr );
  

  // Model data members
  interval = DEFAULT_INTERVAL_MS * (int)thousand;
  
  Geom geom;
  memset( &geom, 0, sizeof(geom));
  geom.size = DEFAULT_SIZE;
  SetGeom( geom );
  
  // assert that Update() is reentrant for this derived model
  thread_safe = true;

  // set default color
  SetColor( stg_lookup_color(DEFAULT_COLOR));
  
  if( world->IsGUI() )
    data_dl = glGenLists(1);
       
  registerOption( &showLaserData );
  registerOption( &showLaserStrikes );
}


ModelLaser::~ModelLaser( void )
{
  if(samples)
    {
      g_free( samples );
      samples = NULL;
    }
}

void ModelLaser::Load( void )
{  
  Model::Load();
  sample_count = wf->ReadInt( wf_entity, "samples", sample_count );
  range_min = wf->ReadLength( wf_entity, "range_min", range_min);
  range_max = wf->ReadLength( wf_entity, "range_max", range_max );
  fov       = wf->ReadAngle( wf_entity, "fov",  fov );
  resolution = wf->ReadInt( wf_entity, "resolution",  resolution );
  
  showLaserData.Load( wf, wf_entity );
  showLaserStrikes.Load( wf, wf_entity );

  if( resolution < 1 )
    {
      PRINT_WARN( "laser resolution set < 1. Forcing to 1" );
      resolution = 1;
    }
}

stg_laser_cfg_t ModelLaser::GetConfig()
{ 
  stg_laser_cfg_t cfg;
  cfg.sample_count = sample_count;
  cfg.range_bounds.min = range_min;
  cfg.range_bounds.max = range_max;
  cfg.fov = fov;
  cfg.resolution = resolution;
  cfg.interval = interval;
  return cfg;
}

void ModelLaser::SetConfig( stg_laser_cfg_t cfg )
{ 
  range_min = cfg.range_bounds.min;
  range_max = cfg.range_bounds.max;
  fov = cfg.fov;
  resolution = cfg.resolution;
  interval = cfg.interval;
}

static bool laser_raytrace_match( Model* hit, 
											 Model* finder,
											 const void* dummy )
{
  // Ignore the model that's looking and things that are invisible to
  // lasers  
  return( (hit != finder) && (hit->vis.laser_return > 0 ) );
}	

void ModelLaser::Update( void )
{     
  double bearing = -fov/2.0;
  double sample_incr = fov / (double)(sample_count-1);

  samples = g_renew( stg_laser_sample_t, samples, sample_count );
  
  Pose rayorg = geom.pose;
  bzero( &rayorg, sizeof(rayorg));
  rayorg.z += geom.size.z/2;
  
  for( unsigned int t=0; t<sample_count; t += resolution )
    {
      rayorg.a = bearing;

      stg_raytrace_result_t sample = 
		  Raytrace( rayorg, 
						range_max,
						laser_raytrace_match,
						NULL,
						true ); // z testing enabled
		
      samples[t].range = sample.range;

      // if we hit a model and it reflects brightly, we set
      // reflectance high, else low
      if( sample.mod && ( sample.mod->vis.laser_return >= LaserBright ) )	
		  samples[t].reflectance = 1;
      else
		  samples[t].reflectance = 0;
		
      // todo - lower bound on range      
      bearing += sample_incr;
    }

  // we may need to interpolate the samples we skipped 
  if( resolution > 1 )
    {
      for( unsigned int t=resolution; t<sample_count; t+=resolution )
		  for( unsigned int g=1; g<resolution; g++ )
			 {
				if( t >= sample_count )
				  break;
				
				// copy the rightmost sample data into this point
				memcpy( &samples[t-g],
						  &samples[t-resolution],
						  sizeof(stg_laser_sample_t));
				
				double left = samples[t].range;
				double right = samples[t-resolution].range;
				
				// linear range interpolation between the left and right samples
				samples[t-g].range = (left-g*(left-right)/resolution);
			 }
    }
  
  data_dirty = true;
  
  Model::Update();
}


void ModelLaser::Startup(  void )
{ 
  Model::Startup();

  PRINT_DEBUG( "laser startup" );

  // start consuming power
  SetWatts( DEFAULT_WATTS );
}

void ModelLaser::Shutdown( void )
{ 
  PRINT_DEBUG( "laser shutdown" );

  // stop consuming power
  SetWatts( 0 );

  // clear the data  
  if(samples)
    {
      g_free( samples );
      samples = NULL;
    }

  Model::Shutdown();
}

void ModelLaser::Print( char* prefix )
{
  Model::Print( prefix );

  printf( "\tRanges[ " );

  if( samples )
    for( unsigned int i=0; i<sample_count; i++ )
      printf( "%.2f ", samples[i].range );
  else
    printf( "<none until subscribed>" );
  puts( " ]" );

  printf( "\tReflectance[ " );

  if( samples )
    for( unsigned int i=0; i<sample_count; i++ )
      printf( "%.2f ", samples[i].reflectance );
  else
    printf( "<none until subscribed>" );

  puts( " ]" );
}


stg_laser_sample_t* ModelLaser::GetSamples( uint32_t* count )
{ 
  if( count ) *count = sample_count;
  return samples;
}

void ModelLaser::SetSamples( stg_laser_sample_t* samples, uint32_t count)
{ 
  this->samples = g_renew( stg_laser_sample_t, this->samples, sample_count );
  memcpy( this->samples, samples, sample_count * sizeof(stg_laser_sample_t));
  this->sample_count = count;
  this->data_dirty = true;
}


void ModelLaser::DataVisualize( Camera* cam )
{
  if( ! (samples && sample_count) )
    return;

  if ( ! (showLaserData || showLaserStrikes) )
    return;
    
  glPushMatrix();
	
  glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

  // we only regenerate the list if there's new data
  if( 1 ) // data_dirty ) // TODO - hmm, why doesn't this work?
    {	    
      data_dirty = false;

      glNewList( data_dl, GL_COMPILE );

      glTranslatef( 0,0, geom.size.z/2.0 ); // shoot the laser beam out at the right height
            
      // DEBUG - draw the origin of the laser beams
      PushColor( 0,0,0,1.0 );
      glPointSize( 4.0 );
      glBegin( GL_POINTS );
      glVertex2f( 0,0 );
      glEnd();
		PopColor();

      // pack the laser hit points into a vertex array for fast rendering
      static float* pts = NULL;
      pts = (float*)g_realloc( pts, 2 * (sample_count+1) * sizeof(float));
      
      pts[0] = 0.0;
      pts[1] = 0.0;
		
		PushColor( 0, 0, 1, 0.5 );
		glDepthMask( GL_FALSE );
		glPointSize( 2 );
		
		for( unsigned int s=0; s<sample_count; s++ )
		  {
			 double ray_angle = (s * (fov / (sample_count-1))) - fov/2.0;
			 pts[2*s+2] = (float)(samples[s].range * cos(ray_angle) );
			 pts[2*s+3] = (float)(samples[s].range * sin(ray_angle) );
			 
			 // if the sample is unusually bright, draw a little blob
			 if( showLaserData && (samples[s].reflectance > 0) )
				{
				  glBegin( GL_POINTS );
				  glVertex2f( pts[2*s+2], pts[2*s+3] );
				  glEnd();
				}			 
		  }
		
		glVertexPointer( 2, GL_FLOAT, 0, pts );      
		
		PopColor();
		
		if( showLaserData )
		  {      			 
			 // draw the filled polygon in transparent blue
			 PushColor( 0, 0, 1, 0.1 );		
			 glDrawArrays( GL_POLYGON, 0, sample_count+1 );
			 PopColor();
		  }
		
      if( showLaserStrikes )
		  {
			 // draw the beam strike points
			 PushColor( 0, 0, 1, 0.8 );
			 glDrawArrays( GL_POINTS, 0, sample_count+1 );
			 PopColor();
		  }
		
      glDepthMask( GL_TRUE );
      glEndList();
    } // end if ( data_dirty )
  
  glCallList( data_dl );
	    
  glPopMatrix();
}

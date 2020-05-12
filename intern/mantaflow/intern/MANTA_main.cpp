/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2016 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup mantaflow
 */

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <zlib.h>

#if OPENVDB == 1
#  include "openvdb/openvdb.h"
#endif

#include "MANTA_main.h"
#include "Python.h"
#include "fluid_script.h"
#include "liquid_script.h"
#include "manta.h"
#include "smoke_script.h"

#include "BLI_fileops.h"
#include "BLI_path_util.h"
#include "BLI_utildefines.h"

#include "DNA_fluid_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

std::atomic<int> MANTA::solverID(0);
int MANTA::with_debug(0);

/* Number of particles that the cache reads at once (with zlib). */
#define PARTICLE_CHUNK 20000
/* Number of mesh nodes that the cache reads at once (with zlib). */
#define NODE_CHUNK 20000
/* Number of mesh triangles that the cache reads at once (with zlib). */
#define TRIANGLE_CHUNK 20000

MANTA::MANTA(int *res, FluidModifierData *mmd) : mCurrentID(++solverID)
{
  if (with_debug)
    std::cout << "FLUID: " << mCurrentID << " with res(" << res[0] << ", " << res[1] << ", "
              << res[2] << ")" << std::endl;

  mmd->domain->fluid = this;

  mUsingLiquid = (mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID);
  mUsingSmoke = (mmd->domain->type == FLUID_DOMAIN_TYPE_GAS);
  mUsingNoise = (mmd->domain->flags & FLUID_DOMAIN_USE_NOISE) && mUsingSmoke;
  mUsingFractions = (mmd->domain->flags & FLUID_DOMAIN_USE_FRACTIONS) && mUsingLiquid;
  mUsingMesh = (mmd->domain->flags & FLUID_DOMAIN_USE_MESH) && mUsingLiquid;
  mUsingMVel = (mmd->domain->flags & FLUID_DOMAIN_USE_SPEED_VECTORS) && mUsingLiquid;
  mUsingGuiding = (mmd->domain->flags & FLUID_DOMAIN_USE_GUIDE);
  mUsingDrops = (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY) && mUsingLiquid;
  mUsingBubbles = (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE) && mUsingLiquid;
  mUsingFloats = (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM) && mUsingLiquid;
  mUsingTracers = (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER) && mUsingLiquid;

  mUsingHeat = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT) && mUsingSmoke;
  mUsingFire = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE) && mUsingSmoke;
  mUsingColors = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS) && mUsingSmoke;
  mUsingObstacle = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE);
  mUsingInvel = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL);
  mUsingOutflow = (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW);

  // Simulation constants
  mTempAmb = 0;  // TODO: Maybe use this later for buoyancy calculation
  mResX = res[0];
  mResY = res[1];
  mResZ = res[2];
  mMaxRes = MAX3(mResX, mResY, mResZ);
  mConstantScaling = 64.0f / mMaxRes;
  mConstantScaling = (mConstantScaling < 1.0f) ? 1.0f : mConstantScaling;
  mTotalCells = mResX * mResY * mResZ;
  mResGuiding = mmd->domain->res;

  // Smoke low res grids
  mDensity = nullptr;
  mShadow = nullptr;
  mHeat = nullptr;
  mVelocityX = nullptr;
  mVelocityY = nullptr;
  mVelocityZ = nullptr;
  mForceX = nullptr;
  mForceY = nullptr;
  mForceZ = nullptr;
  mFlame = nullptr;
  mFuel = nullptr;
  mReact = nullptr;
  mColorR = nullptr;
  mColorG = nullptr;
  mColorB = nullptr;
  mFlags = nullptr;
  mDensityIn = nullptr;
  mHeatIn = nullptr;
  mColorRIn = nullptr;
  mColorGIn = nullptr;
  mColorBIn = nullptr;
  mFuelIn = nullptr;
  mReactIn = nullptr;
  mEmissionIn = nullptr;

  // Smoke high res grids
  mDensityHigh = nullptr;
  mFlameHigh = nullptr;
  mFuelHigh = nullptr;
  mReactHigh = nullptr;
  mColorRHigh = nullptr;
  mColorGHigh = nullptr;
  mColorBHigh = nullptr;
  mTextureU = nullptr;
  mTextureV = nullptr;
  mTextureW = nullptr;
  mTextureU2 = nullptr;
  mTextureV2 = nullptr;
  mTextureW2 = nullptr;

  // Fluid low res grids
  mPhiIn = nullptr;
  mPhiStaticIn = nullptr;
  mPhiOutIn = nullptr;
  mPhiOutStaticIn = nullptr;
  mPhi = nullptr;

  // Mesh
  mMeshNodes = nullptr;
  mMeshTriangles = nullptr;
  mMeshVelocities = nullptr;

  // Fluid obstacle
  mPhiObsIn = nullptr;
  mPhiObsStaticIn = nullptr;
  mNumObstacle = nullptr;
  mObVelocityX = nullptr;
  mObVelocityY = nullptr;
  mObVelocityZ = nullptr;

  // Fluid guiding
  mPhiGuideIn = nullptr;
  mNumGuide = nullptr;
  mGuideVelocityX = nullptr;
  mGuideVelocityY = nullptr;
  mGuideVelocityZ = nullptr;

  // Fluid initial velocity
  mInVelocityX = nullptr;
  mInVelocityY = nullptr;
  mInVelocityZ = nullptr;

  // Secondary particles
  mFlipParticleData = nullptr;
  mFlipParticleVelocity = nullptr;
  mSndParticleData = nullptr;
  mSndParticleVelocity = nullptr;
  mSndParticleLife = nullptr;

  // Cache read success indicators
  mFlipFromFile = false;
  mMeshFromFile = false;
  mParticlesFromFile = false;

  // Setup Mantaflow in Python
  initializeMantaflow();

  // Initialize Mantaflow variables in Python
  // Liquid
  if (mUsingLiquid) {
    initDomain(mmd);
    initLiquid(mmd);
    if (mUsingObstacle)
      initObstacle(mmd);
    if (mUsingInvel)
      initInVelocity(mmd);
    if (mUsingOutflow)
      initOutflow(mmd);

    if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
      mUpresParticle = mmd->domain->particle_scale;
      mResXParticle = mUpresParticle * mResX;
      mResYParticle = mUpresParticle * mResY;
      mResZParticle = mUpresParticle * mResZ;
      mTotalCellsParticles = mResXParticle * mResYParticle * mResZParticle;

      initSndParts(mmd);
      initLiquidSndParts(mmd);
    }

    if (mUsingMesh) {
      mUpresMesh = mmd->domain->mesh_scale;
      mResXMesh = mUpresMesh * mResX;
      mResYMesh = mUpresMesh * mResY;
      mResZMesh = mUpresMesh * mResZ;
      mTotalCellsMesh = mResXMesh * mResYMesh * mResZMesh;

      // Initialize Mantaflow variables in Python
      initMesh(mmd);
      initLiquidMesh(mmd);
    }

    if (mUsingGuiding) {
      mResGuiding = (mmd->domain->guide_parent) ? mmd->domain->guide_res : mmd->domain->res;
      initGuiding(mmd);
    }
    if (mUsingFractions) {
      initFractions(mmd);
    }
  }

  // Smoke
  if (mUsingSmoke) {
    initDomain(mmd);
    initSmoke(mmd);
    if (mUsingHeat)
      initHeat(mmd);
    if (mUsingFire)
      initFire(mmd);
    if (mUsingColors)
      initColors(mmd);
    if (mUsingObstacle)
      initObstacle(mmd);
    if (mUsingInvel)
      initInVelocity(mmd);
    if (mUsingOutflow)
      initOutflow(mmd);

    if (mUsingGuiding) {
      mResGuiding = (mmd->domain->guide_parent) ? mmd->domain->guide_res : mmd->domain->res;
      initGuiding(mmd);
    }

    if (mUsingNoise) {
      int amplify = mmd->domain->noise_scale;
      mResXNoise = amplify * mResX;
      mResYNoise = amplify * mResY;
      mResZNoise = amplify * mResZ;
      mTotalCellsHigh = mResXNoise * mResYNoise * mResZNoise;

      // Initialize Mantaflow variables in Python
      initNoise(mmd);
      initSmokeNoise(mmd);
      if (mUsingFire)
        initFireHigh(mmd);
      if (mUsingColors)
        initColorsHigh(mmd);
    }
  }
  updatePointers();
}

void MANTA::initDomain(FluidModifierData *mmd)
{
  // Vector will hold all python commands that are to be executed
  std::vector<std::string> pythonCommands;

  // Set manta debug level first
  pythonCommands.push_back(manta_import + manta_debuglevel);

  std::ostringstream ss;
  ss << "set_manta_debuglevel(" << with_debug << ")";
  pythonCommands.push_back(ss.str());

  // Now init basic fluid domain
  std::string tmpString = fluid_variables + fluid_solver + fluid_alloc + fluid_cache_helper +
                          fluid_bake_multiprocessing + fluid_bake_data + fluid_bake_noise +
                          fluid_bake_mesh + fluid_bake_particles + fluid_bake_guiding +
                          fluid_file_import + fluid_file_export + fluid_save_data +
                          fluid_load_data + fluid_pre_step + fluid_post_step +
                          fluid_adapt_time_step + fluid_time_stepping;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);
  runPythonString(pythonCommands);
}

void MANTA::initNoise(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_noise + fluid_solver_noise;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initSmoke(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = smoke_variables + smoke_alloc + smoke_adaptive_step + smoke_save_data +
                          smoke_load_data + smoke_step;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initSmokeNoise(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = smoke_variables_noise + smoke_alloc_noise + smoke_wavelet_noise +
                          smoke_save_noise + smoke_load_noise + smoke_step_noise;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingNoise = true;
}

void MANTA::initHeat(FluidModifierData *mmd)
{
  if (!mHeat) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_heat + smoke_with_heat;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingHeat = true;
  }
}

void MANTA::initFire(FluidModifierData *mmd)
{
  if (!mFuel) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_fire + smoke_with_fire;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingFire = true;
  }
}

void MANTA::initFireHigh(FluidModifierData *mmd)
{
  if (!mFuelHigh) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_fire_noise + smoke_with_fire;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingFire = true;
  }
}

void MANTA::initColors(FluidModifierData *mmd)
{
  if (!mColorR) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_colors + smoke_init_colors + smoke_with_colors;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingColors = true;
  }
}

void MANTA::initColorsHigh(FluidModifierData *mmd)
{
  if (!mColorRHigh) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_colors_noise + smoke_init_colors_noise + smoke_with_colors;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingColors = true;
  }
}

void MANTA::initLiquid(FluidModifierData *mmd)
{
  if (!mPhiIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = liquid_variables + liquid_alloc + liquid_init_phi + liquid_save_data +
                            liquid_load_data + liquid_adaptive_step + liquid_step;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingLiquid = true;
  }
}

void MANTA::initMesh(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_mesh + fluid_solver_mesh + liquid_load_mesh;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingMesh = true;
}

void MANTA::initLiquidMesh(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = liquid_alloc_mesh + liquid_step_mesh + liquid_save_mesh;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingMesh = true;
}

void MANTA::initObstacle(FluidModifierData *mmd)
{
  if (!mPhiObsIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_obstacle + fluid_with_obstacle;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingObstacle = true;
  }
}

void MANTA::initGuiding(FluidModifierData *mmd)
{
  if (!mPhiGuideIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_variables_guiding + fluid_solver_guiding + fluid_alloc_guiding +
                            fluid_save_guiding + fluid_load_vel + fluid_load_guiding;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingGuiding = true;
  }
}

void MANTA::initFractions(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_alloc_fractions + fluid_with_fractions;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingFractions = true;
}

void MANTA::initInVelocity(FluidModifierData *mmd)
{
  if (!mInVelocityX) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_invel + fluid_with_invel;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingInvel = true;
  }
}

void MANTA::initOutflow(FluidModifierData *mmd)
{
  if (!mPhiOutIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_outflow + fluid_with_outflow;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingOutflow = true;
  }
}

void MANTA::initSndParts(FluidModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_particles + fluid_solver_particles;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initLiquidSndParts(FluidModifierData *mmd)
{
  if (!mSndParticleData) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = liquid_alloc_particles + liquid_variables_particles +
                            liquid_step_particles + fluid_with_sndparts + liquid_load_particles +
                            liquid_save_particles;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
  }
}

MANTA::~MANTA()
{
  if (with_debug)
    std::cout << "~FLUID: " << mCurrentID << " with res(" << mResX << ", " << mResY << ", "
              << mResZ << ")" << std::endl;

  // Destruction string for Python
  std::string tmpString = "";
  std::vector<std::string> pythonCommands;
  bool result = false;

  tmpString += manta_import;
  tmpString += fluid_delete_all;

  // Leave out mmd argument in parseScript since only looking up IDs
  std::string finalString = parseScript(tmpString);
  pythonCommands.push_back(finalString);
  result = runPythonString(pythonCommands);

  assert(result);
  (void)result;  // not needed in release
}

bool MANTA::runPythonString(std::vector<std::string> commands)
{
  int success = -1;
  PyGILState_STATE gilstate = PyGILState_Ensure();
  for (std::vector<std::string>::iterator it = commands.begin(); it != commands.end(); ++it) {
    std::string command = *it;
    success = PyRun_SimpleString(command.c_str());
  }
  PyGILState_Release(gilstate);

  /* PyRun_SimpleString returns 0 on success, -1 when an error occurred. */
  assert(success == 0);
  return (success != -1);
}

void MANTA::initializeMantaflow()
{
  if (with_debug)
    std::cout << "Fluid: Initializing Mantaflow framework" << std::endl;

  std::string filename = "manta_scene_" + std::to_string(mCurrentID) + ".py";
  std::vector<std::string> fill = std::vector<std::string>();

  // Initialize extension classes and wrappers
  srand(0);
  PyGILState_STATE gilstate = PyGILState_Ensure();
  Pb::setup(filename, fill);  // Namespace from Mantaflow (registry)
  PyGILState_Release(gilstate);
}

void MANTA::terminateMantaflow()
{
  if (with_debug)
    std::cout << "Fluid: Releasing Mantaflow framework" << std::endl;

  PyGILState_STATE gilstate = PyGILState_Ensure();
  Pb::finalize();  // Namespace from Mantaflow (registry)
  PyGILState_Release(gilstate);
}

static std::string getCacheFileEnding(char cache_format)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::getCacheFileEnding()" << std::endl;

  switch (cache_format) {
    case FLUID_DOMAIN_FILE_UNI:
      return FLUID_DOMAIN_EXTENSION_UNI;
    case FLUID_DOMAIN_FILE_OPENVDB:
      return FLUID_DOMAIN_EXTENSION_OPENVDB;
    case FLUID_DOMAIN_FILE_RAW:
      return FLUID_DOMAIN_EXTENSION_RAW;
    case FLUID_DOMAIN_FILE_BIN_OBJECT:
      return FLUID_DOMAIN_EXTENSION_BINOBJ;
    case FLUID_DOMAIN_FILE_OBJECT:
      return FLUID_DOMAIN_EXTENSION_OBJ;
    default:
      std::cerr << "Fluid Error -- Could not find file extension. Using default file extension."
                << std::endl;
      return FLUID_DOMAIN_EXTENSION_UNI;
  }
}

std::string MANTA::getRealValue(const std::string &varName, FluidModifierData *mmd)
{
  std::ostringstream ss;
  bool is2D = false;
  int tmpVar;
  float tmpFloat;

  if (varName == "ID") {
    ss << mCurrentID;
    return ss.str();
  }

  if (!mmd) {
    std::cerr << "Fluid Error -- Invalid modifier data." << std::endl;
    ss << "ERROR - INVALID MODIFIER DATA";
    return ss.str();
  }

  is2D = (mmd->domain->solver_res == 2);

  if (varName == "USING_SMOKE")
    ss << ((mmd->domain->type == FLUID_DOMAIN_TYPE_GAS) ? "True" : "False");
  else if (varName == "USING_LIQUID")
    ss << ((mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID) ? "True" : "False");
  else if (varName == "USING_COLORS")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS ? "True" : "False");
  else if (varName == "USING_HEAT")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT ? "True" : "False");
  else if (varName == "USING_FIRE")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE ? "True" : "False");
  else if (varName == "USING_NOISE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_NOISE ? "True" : "False");
  else if (varName == "USING_OBSTACLE")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE ? "True" : "False");
  else if (varName == "USING_GUIDING")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_GUIDE ? "True" : "False");
  else if (varName == "USING_INVEL")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL ? "True" : "False");
  else if (varName == "USING_OUTFLOW")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW ? "True" : "False");
  else if (varName == "USING_LOG_DISSOLVE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_DISSOLVE_LOG ? "True" : "False");
  else if (varName == "USING_DISSOLVE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_DISSOLVE ? "True" : "False");
  else if (varName == "SOLVER_DIM")
    ss << mmd->domain->solver_res;
  else if (varName == "DO_OPEN") {
    tmpVar = (FLUID_DOMAIN_BORDER_BACK | FLUID_DOMAIN_BORDER_FRONT | FLUID_DOMAIN_BORDER_LEFT |
              FLUID_DOMAIN_BORDER_RIGHT | FLUID_DOMAIN_BORDER_BOTTOM | FLUID_DOMAIN_BORDER_TOP);
    ss << (((mmd->domain->border_collisions & tmpVar) == tmpVar) ? "False" : "True");
  }
  else if (varName == "BOUND_CONDITIONS") {
    if (mmd->domain->solver_res == 2) {
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_LEFT) == 0)
        ss << "x";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_RIGHT) == 0)
        ss << "X";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_FRONT) == 0)
        ss << "y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BACK) == 0)
        ss << "Y";
    }
    if (mmd->domain->solver_res == 3) {
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_LEFT) == 0)
        ss << "x";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_RIGHT) == 0)
        ss << "X";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_FRONT) == 0)
        ss << "y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BACK) == 0)
        ss << "Y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BOTTOM) == 0)
        ss << "z";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_TOP) == 0)
        ss << "Z";
    }
  }
  else if (varName == "BOUNDARY_WIDTH")
    ss << mmd->domain->boundary_width;
  else if (varName == "RES")
    ss << mMaxRes;
  else if (varName == "RESX")
    ss << mResX;
  else if (varName == "RESY")
    if (is2D) {
      ss << mResZ;
    }
    else {
      ss << mResY;
    }
  else if (varName == "RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZ;
    }
  }
  else if (varName == "FRAME_LENGTH")
    ss << mmd->domain->frame_length;
  else if (varName == "CFL")
    ss << mmd->domain->cfl_condition;
  else if (varName == "DT")
    ss << mmd->domain->dt;
  else if (varName == "TIMESTEPS_MIN")
    ss << mmd->domain->timesteps_minimum;
  else if (varName == "TIMESTEPS_MAX")
    ss << mmd->domain->timesteps_maximum;
  else if (varName == "TIME_TOTAL")
    ss << mmd->domain->time_total;
  else if (varName == "TIME_PER_FRAME")
    ss << mmd->domain->time_per_frame;
  else if (varName == "VORTICITY")
    ss << mmd->domain->vorticity / mConstantScaling;
  else if (varName == "FLAME_VORTICITY")
    ss << mmd->domain->flame_vorticity / mConstantScaling;
  else if (varName == "NOISE_SCALE")
    ss << mmd->domain->noise_scale;
  else if (varName == "MESH_SCALE")
    ss << mmd->domain->mesh_scale;
  else if (varName == "PARTICLE_SCALE")
    ss << mmd->domain->particle_scale;
  else if (varName == "NOISE_RESX")
    ss << mResXNoise;
  else if (varName == "NOISE_RESY") {
    if (is2D) {
      ss << mResZNoise;
    }
    else {
      ss << mResYNoise;
    }
  }
  else if (varName == "NOISE_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZNoise;
    }
  }
  else if (varName == "MESH_RESX")
    ss << mResXMesh;
  else if (varName == "MESH_RESY") {
    if (is2D) {
      ss << mResZMesh;
    }
    else {
      ss << mResYMesh;
    }
  }
  else if (varName == "MESH_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZMesh;
    }
  }
  else if (varName == "PARTICLE_RESX")
    ss << mResXParticle;
  else if (varName == "PARTICLE_RESY") {
    if (is2D) {
      ss << mResZParticle;
    }
    else {
      ss << mResYParticle;
    }
  }
  else if (varName == "PARTICLE_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZParticle;
    }
  }
  else if (varName == "GUIDING_RESX")
    ss << mResGuiding[0];
  else if (varName == "GUIDING_RESY") {
    if (is2D) {
      ss << mResGuiding[2];
    }
    else {
      ss << mResGuiding[1];
    }
  }
  else if (varName == "GUIDING_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResGuiding[2];
    }
  }
  else if (varName == "MIN_RESX")
    ss << mmd->domain->res_min[0];
  else if (varName == "MIN_RESY")
    ss << mmd->domain->res_min[1];
  else if (varName == "MIN_RESZ")
    ss << mmd->domain->res_min[2];
  else if (varName == "BASE_RESX")
    ss << mmd->domain->base_res[0];
  else if (varName == "BASE_RESY")
    ss << mmd->domain->base_res[1];
  else if (varName == "BASE_RESZ")
    ss << mmd->domain->base_res[2];
  else if (varName == "WLT_STR")
    ss << mmd->domain->noise_strength;
  else if (varName == "NOISE_POSSCALE")
    ss << mmd->domain->noise_pos_scale;
  else if (varName == "NOISE_TIMEANIM")
    ss << mmd->domain->noise_time_anim;
  else if (varName == "COLOR_R")
    ss << mmd->domain->active_color[0];
  else if (varName == "COLOR_G")
    ss << mmd->domain->active_color[1];
  else if (varName == "COLOR_B")
    ss << mmd->domain->active_color[2];
  else if (varName == "BUOYANCY_ALPHA")
    ss << mmd->domain->alpha;
  else if (varName == "BUOYANCY_BETA")
    ss << mmd->domain->beta;
  else if (varName == "DISSOLVE_SPEED")
    ss << mmd->domain->diss_speed;
  else if (varName == "BURNING_RATE")
    ss << mmd->domain->burning_rate;
  else if (varName == "FLAME_SMOKE")
    ss << mmd->domain->flame_smoke;
  else if (varName == "IGNITION_TEMP")
    ss << mmd->domain->flame_ignition;
  else if (varName == "MAX_TEMP")
    ss << mmd->domain->flame_max_temp;
  else if (varName == "FLAME_SMOKE_COLOR_X")
    ss << mmd->domain->flame_smoke_color[0];
  else if (varName == "FLAME_SMOKE_COLOR_Y")
    ss << mmd->domain->flame_smoke_color[1];
  else if (varName == "FLAME_SMOKE_COLOR_Z")
    ss << mmd->domain->flame_smoke_color[2];
  else if (varName == "CURRENT_FRAME")
    ss << mmd->time;
  else if (varName == "START_FRAME")
    ss << mmd->domain->cache_frame_start;
  else if (varName == "END_FRAME")
    ss << mmd->domain->cache_frame_end;
  else if (varName == "CACHE_DATA_FORMAT")
    ss << getCacheFileEnding(mmd->domain->cache_data_format);
  else if (varName == "CACHE_MESH_FORMAT")
    ss << getCacheFileEnding(mmd->domain->cache_mesh_format);
  else if (varName == "CACHE_NOISE_FORMAT")
    ss << getCacheFileEnding(mmd->domain->cache_noise_format);
  else if (varName == "CACHE_PARTICLE_FORMAT")
    ss << getCacheFileEnding(mmd->domain->cache_particle_format);
  else if (varName == "SIMULATION_METHOD") {
    if (mmd->domain->simulation_method & FLUID_DOMAIN_METHOD_FLIP) {
      ss << "'FLIP'";
    }
    else if (mmd->domain->simulation_method & FLUID_DOMAIN_METHOD_APIC) {
      ss << "'APIC'";
    }
    else {
      ss << "'NONE'";
    }
  }
  else if (varName == "FLIP_RATIO")
    ss << mmd->domain->flip_ratio;
  else if (varName == "PARTICLE_RANDOMNESS")
    ss << mmd->domain->particle_randomness;
  else if (varName == "PARTICLE_NUMBER")
    ss << mmd->domain->particle_number;
  else if (varName == "PARTICLE_MINIMUM")
    ss << mmd->domain->particle_minimum;
  else if (varName == "PARTICLE_MAXIMUM")
    ss << mmd->domain->particle_maximum;
  else if (varName == "PARTICLE_RADIUS")
    ss << mmd->domain->particle_radius;
  else if (varName == "FRACTIONS_THRESHOLD")
    ss << mmd->domain->fractions_threshold;
  else if (varName == "MESH_CONCAVE_UPPER")
    ss << mmd->domain->mesh_concave_upper;
  else if (varName == "MESH_CONCAVE_LOWER")
    ss << mmd->domain->mesh_concave_lower;
  else if (varName == "MESH_PARTICLE_RADIUS")
    ss << mmd->domain->mesh_particle_radius;
  else if (varName == "MESH_SMOOTHEN_POS")
    ss << mmd->domain->mesh_smoothen_pos;
  else if (varName == "MESH_SMOOTHEN_NEG")
    ss << mmd->domain->mesh_smoothen_neg;
  else if (varName == "USING_MESH")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_MESH ? "True" : "False");
  else if (varName == "USING_IMPROVED_MESH")
    ss << (mmd->domain->mesh_generator == FLUID_DOMAIN_MESH_IMPROVED ? "True" : "False");
  else if (varName == "PARTICLE_BAND_WIDTH")
    ss << mmd->domain->particle_band_width;
  else if (varName == "SNDPARTICLE_TAU_MIN_WC")
    ss << mmd->domain->sndparticle_tau_min_wc;
  else if (varName == "SNDPARTICLE_TAU_MAX_WC")
    ss << mmd->domain->sndparticle_tau_max_wc;
  else if (varName == "SNDPARTICLE_TAU_MIN_TA")
    ss << mmd->domain->sndparticle_tau_min_ta;
  else if (varName == "SNDPARTICLE_TAU_MAX_TA")
    ss << mmd->domain->sndparticle_tau_max_ta;
  else if (varName == "SNDPARTICLE_TAU_MIN_K")
    ss << mmd->domain->sndparticle_tau_min_k;
  else if (varName == "SNDPARTICLE_TAU_MAX_K")
    ss << mmd->domain->sndparticle_tau_max_k;
  else if (varName == "SNDPARTICLE_K_WC")
    ss << mmd->domain->sndparticle_k_wc;
  else if (varName == "SNDPARTICLE_K_TA")
    ss << mmd->domain->sndparticle_k_ta;
  else if (varName == "SNDPARTICLE_K_B")
    ss << mmd->domain->sndparticle_k_b;
  else if (varName == "SNDPARTICLE_K_D")
    ss << mmd->domain->sndparticle_k_d;
  else if (varName == "SNDPARTICLE_L_MIN")
    ss << mmd->domain->sndparticle_l_min;
  else if (varName == "SNDPARTICLE_L_MAX")
    ss << mmd->domain->sndparticle_l_max;
  else if (varName == "SNDPARTICLE_BOUNDARY_DELETE")
    ss << (mmd->domain->sndparticle_boundary == SNDPARTICLE_BOUNDARY_DELETE);
  else if (varName == "SNDPARTICLE_BOUNDARY_PUSHOUT")
    ss << (mmd->domain->sndparticle_boundary == SNDPARTICLE_BOUNDARY_PUSHOUT);
  else if (varName == "SNDPARTICLE_POTENTIAL_RADIUS")
    ss << mmd->domain->sndparticle_potential_radius;
  else if (varName == "SNDPARTICLE_UPDATE_RADIUS")
    ss << mmd->domain->sndparticle_update_radius;
  else if (varName == "LIQUID_SURFACE_TENSION")
    ss << mmd->domain->surface_tension;
  else if (varName == "FLUID_VISCOSITY")
    ss << mmd->domain->viscosity_base * pow(10.0f, -mmd->domain->viscosity_exponent);
  else if (varName == "FLUID_DOMAIN_SIZE") {
    tmpFloat = MAX3(
        mmd->domain->global_size[0], mmd->domain->global_size[1], mmd->domain->global_size[2]);
    ss << tmpFloat;
  }
  else if (varName == "SNDPARTICLE_TYPES") {
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY) {
      ss << "PtypeSpray";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeBubble";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeFoam";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeTracer";
    }
    if (ss.str().empty())
      ss << "0";
  }
  else if (varName == "USING_SNDPARTS") {
    tmpVar = (FLUID_DOMAIN_PARTICLE_SPRAY | FLUID_DOMAIN_PARTICLE_BUBBLE |
              FLUID_DOMAIN_PARTICLE_FOAM | FLUID_DOMAIN_PARTICLE_TRACER);
    ss << (((mmd->domain->particle_type & tmpVar)) ? "True" : "False");
  }
  else if (varName == "GUIDING_ALPHA")
    ss << mmd->domain->guide_alpha;
  else if (varName == "GUIDING_BETA")
    ss << mmd->domain->guide_beta;
  else if (varName == "GUIDING_FACTOR")
    ss << mmd->domain->guide_vel_factor;
  else if (varName == "GRAVITY_X")
    ss << mmd->domain->gravity[0];
  else if (varName == "GRAVITY_Y")
    ss << mmd->domain->gravity[1];
  else if (varName == "GRAVITY_Z")
    ss << mmd->domain->gravity[2];
  else if (varName == "CACHE_DIR")
    ss << mmd->domain->cache_directory;
  else if (varName == "CACHE_RESUMABLE")
    ss << (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL ? "False" : "True");
  else if (varName == "USING_ADAPTIVETIME")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_ADAPTIVE_TIME ? "True" : "False");
  else if (varName == "USING_SPEEDVECTORS")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_SPEED_VECTORS ? "True" : "False");
  else if (varName == "USING_FRACTIONS")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_FRACTIONS ? "True" : "False");
  else if (varName == "DELETE_IN_OBSTACLE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_DELETE_IN_OBSTACLE ? "True" : "False");
  else if (varName == "USING_DIFFUSION")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_DIFFUSION ? "True" : "False");
  else
    std::cerr << "Fluid Error -- Unknown option: " << varName << std::endl;
  return ss.str();
}

std::string MANTA::parseLine(const std::string &line, FluidModifierData *mmd)
{
  if (line.size() == 0)
    return "";
  std::string res = "";
  int currPos = 0, start_del = 0, end_del = -1;
  bool readingVar = false;
  const char delimiter = '$';
  while (currPos < line.size()) {
    if (line[currPos] == delimiter && !readingVar) {
      readingVar = true;
      start_del = currPos + 1;
      res += line.substr(end_del + 1, currPos - end_del - 1);
    }
    else if (line[currPos] == delimiter && readingVar) {
      readingVar = false;
      end_del = currPos;
      res += getRealValue(line.substr(start_del, currPos - start_del), mmd);
    }
    currPos++;
  }
  res += line.substr(end_del + 1, line.size() - end_del);
  return res;
}

std::string MANTA::parseScript(const std::string &setup_string, FluidModifierData *mmd)
{
  std::istringstream f(setup_string);
  std::ostringstream res;
  std::string line = "";
  while (getline(f, line)) {
    res << parseLine(line, mmd) << "\n";
  }
  return res.str();
}

bool MANTA::updateFlipStructures(FluidModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateFlipStructures()" << std::endl;

  mFlipFromFile = false;

  if (!mUsingLiquid)
    return false;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return false;

  int result = 0;
  int expected = 0; /* Expected number of read successes for this frame. */

  /* Ensure empty data structures at start. */
  if (!mFlipParticleData || !mFlipParticleVelocity)
    return false;

  mFlipParticleData->clear();
  mFlipParticleVelocity->clear();

  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_PP, pformat.c_str(), framenr);

  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateParticlesFromFile(file, false, false);
    assert(result == expected);
  }

  file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_PVEL, pformat.c_str(), framenr);
  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateParticlesFromFile(file, false, true);
    assert(result == expected);
  }

  return mFlipFromFile = (result == expected);
}

bool MANTA::updateMeshStructures(FluidModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateMeshStructures()" << std::endl;

  mMeshFromFile = false;

  if (!mUsingMesh)
    return false;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return false;

  int result = 0;
  int expected = 0; /* Expected number of read successes for this frame. */

  /* Ensure empty data structures at start. */
  if (!mMeshNodes || !mMeshTriangles)
    return false;

  mMeshNodes->clear();
  mMeshTriangles->clear();

  if (mMeshVelocities)
    mMeshVelocities->clear();

  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);
  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string file = getFile(mmd, FLUID_DOMAIN_DIR_MESH, FLUID_DOMAIN_FILE_MESH, mformat, framenr);

  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateMeshFromFile(file);
    assert(result == expected);
  }

  if (mUsingMVel) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_MESH, FLUID_DOMAIN_FILE_MESHVEL, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateMeshFromFile(file);
      assert(result == expected);
    }
  }

  return mMeshFromFile = (result == expected);
}

bool MANTA::updateParticleStructures(FluidModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateParticleStructures()" << std::endl;

  mParticlesFromFile = false;

  if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers)
    return false;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return false;

  int result = 0;
  int expected = 0; /* Expected number of read successes for this frame. */

  /* Ensure empty data structures at start. */
  if (!mSndParticleData || !mSndParticleVelocity || !mSndParticleLife)
    return false;

  mSndParticleData->clear();
  mSndParticleVelocity->clear();
  mSndParticleLife->clear();

  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_PARTICLES, FLUID_DOMAIN_FILE_PPSND, pformat, framenr);

  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateParticlesFromFile(file, true, false);
    assert(result == expected);
  }

  file = getFile(mmd, FLUID_DOMAIN_DIR_PARTICLES, FLUID_DOMAIN_FILE_PVELSND, pformat, framenr);
  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateParticlesFromFile(file, true, true);
    assert(result == expected);
  }

  file = getFile(mmd, FLUID_DOMAIN_DIR_PARTICLES, FLUID_DOMAIN_FILE_PLIFESND, pformat, framenr);
  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateParticlesFromFile(file, true, false);
    assert(result == expected);
  }

  return mParticlesFromFile = (result == expected);
}

bool MANTA::updateSmokeStructures(FluidModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateGridStructures()" << std::endl;

  mSmokeFromFile = false;

  if (!mUsingSmoke)
    return false;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return false;

  int result = 0;
  int expected = 0; /* Expected number of read successes for this frame. */

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_DENSITY, dformat, framenr);

  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateGridFromFile(file, mDensity, false);
    assert(result == expected);
  }

  file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_SHADOW, dformat, framenr);
  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateGridFromFile(file, mShadow, false);
    assert(result == expected);
  }

  if (mUsingHeat) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_HEAT, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mHeat, false);
      assert(result == expected);
    }
  }

  if (mUsingColors) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_COLORR, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorR, false);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_COLORG, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorG, false);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_COLORB, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorB, false);
      assert(result == expected);
    }
  }

  if (mUsingFire) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_FLAME, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mFlame, false);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_FUEL, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mFuel, false);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_REACT, dformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mReact, false);
      assert(result == expected);
    }
  }

  return mSmokeFromFile = (result == expected);
}

bool MANTA::updateNoiseStructures(FluidModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateNoiseStructures()" << std::endl;

  mNoiseFromFile = false;

  if (!mUsingSmoke || !mUsingNoise)
    return false;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return false;

  int result = 0;
  int expected = 0; /* Expected number of read successes for this frame. */

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_DENSITYNOISE, nformat, framenr);

  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateGridFromFile(file, mDensityHigh, true);
    assert(result == expected);
  }

  file = getFile(mmd, FLUID_DOMAIN_DIR_DATA, FLUID_DOMAIN_FILE_SHADOW, dformat, framenr);
  expected += 1;
  if (BLI_exists(file.c_str())) {
    result += updateGridFromFile(file, mShadow, false);
    assert(result == expected);
  }

  if (mUsingColors) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_COLORRNOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorRHigh, true);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_COLORGNOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorGHigh, true);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_COLORBNOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mColorBHigh, true);
      assert(result == expected);
    }
  }

  if (mUsingFire) {
    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_FLAMENOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mFlameHigh, true);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_FUELNOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mFuelHigh, true);
      assert(result == expected);
    }

    file = getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_REACTNOISE, nformat, framenr);
    expected += 1;
    if (BLI_exists(file.c_str())) {
      result += updateGridFromFile(file, mReactHigh, true);
      assert(result == expected);
    }
  }

  return mNoiseFromFile = (result == expected);
}

/* Dirty hack: Needed to format paths from python code that is run via PyRun_SimpleString */
static std::string escapeSlashes(std::string const &s)
{
  std::string result = "";
  for (std::string::const_iterator i = s.begin(), end = s.end(); i != end; ++i) {
    unsigned char c = *i;
    if (c == '\\')
      result += "\\\\";
    else
      result += c;
  }
  return result;
}

bool MANTA::writeConfiguration(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::writeConfiguration()" << std::endl;

  FluidDomainSettings *mds = mmd->domain;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_CONFIG);
  std::string format = FLUID_DOMAIN_EXTENSION_UNI;
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_CONFIG, FLUID_DOMAIN_FILE_CONFIG, format, framenr);

  /* Create 'config' subdir if it does not exist already. */
  BLI_dir_create_recursive(directory.c_str());

  gzFile gzf = (gzFile)BLI_gzopen(file.c_str(), "wb1");  // do some compression
  if (!gzf) {
    std::cerr << "Fluid Error -- Cannot open file " << file << std::endl;
    return false;
  }

  gzwrite(gzf, &mds->active_fields, sizeof(int));
  gzwrite(gzf, &mds->res, 3 * sizeof(int));
  gzwrite(gzf, &mds->dx, sizeof(float));
  gzwrite(gzf, &mds->dt, sizeof(float));
  gzwrite(gzf, &mds->p0, 3 * sizeof(float));
  gzwrite(gzf, &mds->p1, 3 * sizeof(float));
  gzwrite(gzf, &mds->dp0, 3 * sizeof(float));
  gzwrite(gzf, &mds->shift, 3 * sizeof(int));
  gzwrite(gzf, &mds->obj_shift_f, 3 * sizeof(float));
  gzwrite(gzf, &mds->obmat, 16 * sizeof(float));
  gzwrite(gzf, &mds->base_res, 3 * sizeof(int));
  gzwrite(gzf, &mds->res_min, 3 * sizeof(int));
  gzwrite(gzf, &mds->res_max, 3 * sizeof(int));
  gzwrite(gzf, &mds->active_color, 3 * sizeof(float));

  return (gzclose(gzf) == Z_OK);
}

bool MANTA::writeData(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::writeData()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_DATA);
  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  ss.str("");
  ss << "fluid_save_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
     << ", '" << dformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  if (mUsingSmoke) {
    ss.str("");
    ss << "smoke_save_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << dformat << "', " << resumable_cache << ")";
    pythonCommands.push_back(ss.str());
  }
  if (mUsingLiquid) {
    ss.str("");
    ss << "liquid_save_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << dformat << "', " << resumable_cache << ")";
    pythonCommands.push_back(ss.str());
  }
  return runPythonString(pythonCommands);
}

bool MANTA::writeNoise(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::writeNoise()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_NOISE);
  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  if (mUsingSmoke && mUsingNoise) {
    ss.str("");
    ss << "smoke_save_noise_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << nformat << "', " << resumable_cache << ")";
    pythonCommands.push_back(ss.str());
  }
  return runPythonString(pythonCommands);
}

bool MANTA::readConfiguration(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readConfiguration()" << std::endl;

  FluidDomainSettings *mds = mmd->domain;
  float dummy;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_CONFIG);
  std::string format = FLUID_DOMAIN_EXTENSION_UNI;
  std::string file = getFile(
      mmd, FLUID_DOMAIN_DIR_CONFIG, FLUID_DOMAIN_FILE_CONFIG, format, framenr);

  if (!hasConfig(mmd, framenr))
    return false;

  gzFile gzf = (gzFile)BLI_gzopen(file.c_str(), "rb");  // do some compression
  if (!gzf) {
    std::cerr << "Fluid Error -- Cannot open file " << file << std::endl;
    return false;
  }

  gzread(gzf, &mds->active_fields, sizeof(int));
  gzread(gzf, &mds->res, 3 * sizeof(int));
  gzread(gzf, &mds->dx, sizeof(float));
  gzread(gzf, &dummy, sizeof(float));  // dt not needed right now
  gzread(gzf, &mds->p0, 3 * sizeof(float));
  gzread(gzf, &mds->p1, 3 * sizeof(float));
  gzread(gzf, &mds->dp0, 3 * sizeof(float));
  gzread(gzf, &mds->shift, 3 * sizeof(int));
  gzread(gzf, &mds->obj_shift_f, 3 * sizeof(float));
  gzread(gzf, &mds->obmat, 16 * sizeof(float));
  gzread(gzf, &mds->base_res, 3 * sizeof(int));
  gzread(gzf, &mds->res_min, 3 * sizeof(int));
  gzread(gzf, &mds->res_max, 3 * sizeof(int));
  gzread(gzf, &mds->active_color, 3 * sizeof(float));
  mds->total_cells = mds->res[0] * mds->res[1] * mds->res[2];

  return (gzclose(gzf) == Z_OK);
}

bool MANTA::readData(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readData()" << std::endl;

  if (!mUsingSmoke && !mUsingLiquid)
    return false;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;
  bool result = true;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_DATA);
  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  /* Sanity check: Are cache files present? */
  if (!hasData(mmd, framenr))
    return false;

  ss.str("");
  ss << "fluid_load_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
     << ", '" << dformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  if (mUsingSmoke) {
    ss.str("");
    ss << "smoke_load_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << dformat << "', " << resumable_cache << ")";
    pythonCommands.push_back(ss.str());
    result &= runPythonString(pythonCommands);
  }
  if (mUsingLiquid) {
    ss.str("");
    ss << "liquid_load_data_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << dformat << "', " << resumable_cache << ")";
    pythonCommands.push_back(ss.str());
    result &= runPythonString(pythonCommands);
  }
  return result;
}

bool MANTA::readNoise(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readNoise()" << std::endl;

  if (!mUsingSmoke || !mUsingNoise)
    return false;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_NOISE);
  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  /* Sanity check: Are cache files present? */
  if (!hasNoise(mmd, framenr))
    return false;

  ss.str("");
  ss << "smoke_load_noise_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
     << ", '" << nformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

/* Deprecated! This function reads mesh data via the Manta Python API.
 * MANTA:updateMeshStructures() reads cache files directly from disk
 * and is preferred due to its better performance. */
bool MANTA::readMesh(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readMesh()" << std::endl;

  if (!mUsingLiquid || !mUsingMesh)
    return false;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_MESH);
  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);
  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);

  /* Sanity check: Are cache files present? */
  if (!hasMesh(mmd, framenr))
    return false;

  ss.str("");
  ss << "liquid_load_mesh_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
     << ", '" << mformat << "')";
  pythonCommands.push_back(ss.str());

  if (mUsingMVel) {
    ss.str("");
    ss << "liquid_load_meshvel_" << mCurrentID << "('" << escapeSlashes(directory) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
  }

  return runPythonString(pythonCommands);
}

/* Deprecated! This function reads particle data via the Manta Python API.
 * MANTA:updateParticleStructures() reads cache files directly from disk
 * and is preferred due to its better performance. */
bool MANTA::readParticles(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readParticles()" << std::endl;

  if (!mUsingLiquid)
    return false;
  if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers)
    return false;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = getDirectory(mmd, FLUID_DOMAIN_DIR_PARTICLES);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  /* Sanity check: Are cache files present? */
  if (!hasParticles(mmd, framenr))
    return false;

  ss.str("");
  ss << "liquid_load_particles_" << mCurrentID << "('" << escapeSlashes(directory) << "', "
     << framenr << ", '" << pformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::readGuiding(FluidModifierData *mmd, int framenr, bool sourceDomain)
{
  if (with_debug)
    std::cout << "MANTA::readGuiding()" << std::endl;

  if (!mUsingGuiding)
    return false;
  if (!mmd->domain)
    return false;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  std::string directory = (sourceDomain) ? getDirectory(mmd, FLUID_DOMAIN_DIR_DATA) :
                                           getDirectory(mmd, FLUID_DOMAIN_DIR_GUIDE);
  std::string gformat = getCacheFileEnding(mmd->domain->cache_data_format);

  /* Sanity check: Are cache files present? */
  if (!hasGuiding(mmd, framenr, sourceDomain))
    return false;

  if (sourceDomain) {
    ss.str("");
    ss << "fluid_load_vel_" << mCurrentID << "('" << escapeSlashes(directory) << "', " << framenr
       << ", '" << gformat << "')";
  }
  else {
    ss.str("");
    ss << "fluid_load_guiding_" << mCurrentID << "('" << escapeSlashes(directory) << "', "
       << framenr << ", '" << gformat << "')";
  }
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::bakeData(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeData()" << std::endl;

  std::string tmpString, finalString;
  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirGuiding[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirGuiding[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  std::string gformat = dformat;  // Use same data format for guiding format

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                nullptr);
  BLI_path_join(cacheDirGuiding,
                sizeof(cacheDirGuiding),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_GUIDE,
                nullptr);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirGuiding);

  ss.str("");
  ss << "bake_fluid_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirGuiding) << "', " << framenr << ", '" << dformat << "', '" << pformat
     << "', '" << gformat << "')";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::bakeNoise(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeNoise()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirNoise[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirNoise[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                nullptr);
  BLI_path_join(cacheDirNoise,
                sizeof(cacheDirNoise),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_NOISE,
                nullptr);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirNoise);

  ss.str("");
  ss << "bake_noise_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirNoise) << "', " << framenr << ", '" << dformat << "', '" << nformat
     << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::bakeMesh(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeMesh()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirMesh[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirMesh[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                nullptr);
  BLI_path_join(cacheDirMesh,
                sizeof(cacheDirMesh),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_MESH,
                nullptr);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirMesh);

  ss.str("");
  ss << "bake_mesh_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirMesh) << "', " << framenr << ", '" << dformat << "', '" << mformat
     << "', '" << pformat << "')";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::bakeParticles(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeParticles()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirParticles[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirParticles[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                nullptr);
  BLI_path_join(cacheDirParticles,
                sizeof(cacheDirParticles),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_PARTICLES,
                nullptr);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirParticles);

  ss.str("");
  ss << "bake_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirParticles) << "', " << framenr << ", '" << dformat << "', '"
     << pformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::bakeGuiding(FluidModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeGuiding()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirGuiding[FILE_MAX];
  cacheDirGuiding[0] = '\0';

  std::string gformat = getCacheFileEnding(mmd->domain->cache_data_format);

  bool final_cache = (mmd->domain->cache_type == FLUID_DOMAIN_CACHE_FINAL);
  std::string resumable_cache = (final_cache) ? "False" : "True";

  BLI_path_join(cacheDirGuiding,
                sizeof(cacheDirGuiding),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_GUIDE,
                nullptr);
  BLI_path_make_safe(cacheDirGuiding);

  ss.str("");
  ss << "bake_guiding_" << mCurrentID << "('" << escapeSlashes(cacheDirGuiding) << "', " << framenr
     << ", '" << gformat << "', " << resumable_cache << ")";
  pythonCommands.push_back(ss.str());

  return runPythonString(pythonCommands);
}

bool MANTA::updateVariables(FluidModifierData *mmd)
{
  std::string tmpString, finalString;
  std::vector<std::string> pythonCommands;

  tmpString += fluid_variables;
  if (mUsingSmoke)
    tmpString += smoke_variables;
  if (mUsingLiquid)
    tmpString += liquid_variables;
  if (mUsingGuiding)
    tmpString += fluid_variables_guiding;
  if (mUsingNoise) {
    tmpString += fluid_variables_noise;
    tmpString += smoke_variables_noise;
    tmpString += smoke_wavelet_noise;
  }
  if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
    tmpString += fluid_variables_particles;
    tmpString += liquid_variables_particles;
  }
  if (mUsingMesh)
    tmpString += fluid_variables_mesh;

  finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  return runPythonString(pythonCommands);
}

void MANTA::exportSmokeScript(FluidModifierData *mmd)
{
  if (with_debug)
    std::cout << "MANTA::exportSmokeScript()" << std::endl;

  char cacheDir[FILE_MAX] = "\0";
  char cacheDirScript[FILE_MAX] = "\0";

  BLI_path_join(
      cacheDir, sizeof(cacheDir), mmd->domain->cache_directory, FLUID_DOMAIN_DIR_SCRIPT, nullptr);
  BLI_path_make_safe(cacheDir);
  /* Create 'script' subdir if it does not exist already */
  BLI_dir_create_recursive(cacheDir);
  BLI_path_join(
      cacheDirScript, sizeof(cacheDirScript), cacheDir, FLUID_DOMAIN_SMOKE_SCRIPT, nullptr);
  BLI_path_make_safe(cacheDir);

  bool noise = mmd->domain->flags & FLUID_DOMAIN_USE_NOISE;
  bool heat = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT;
  bool colors = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS;
  bool fire = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE;
  bool obstacle = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE;
  bool guiding = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_GUIDE;
  bool invel = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL;
  bool outflow = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW;

  std::string manta_script;

  // Libraries
  manta_script += header_libraries + manta_import;

  // Variables
  manta_script += header_variables + fluid_variables + smoke_variables;
  if (noise) {
    manta_script += fluid_variables_noise + smoke_variables_noise;
  }
  if (guiding)
    manta_script += fluid_variables_guiding;

  // Solvers
  manta_script += header_solvers + fluid_solver;
  if (noise)
    manta_script += fluid_solver_noise;
  if (guiding)
    manta_script += fluid_solver_guiding;

  // Grids
  manta_script += header_grids + fluid_alloc + smoke_alloc;
  if (noise) {
    manta_script += smoke_alloc_noise;
    if (colors)
      manta_script += smoke_alloc_colors_noise;
    if (fire)
      manta_script += smoke_alloc_fire_noise;
  }
  if (heat)
    manta_script += smoke_alloc_heat;
  if (colors)
    manta_script += smoke_alloc_colors;
  if (fire)
    manta_script += smoke_alloc_fire;
  if (guiding)
    manta_script += fluid_alloc_guiding;
  if (obstacle)
    manta_script += fluid_alloc_obstacle;
  if (invel)
    manta_script += fluid_alloc_invel;
  if (outflow)
    manta_script += fluid_alloc_outflow;

  // Noise field
  if (noise)
    manta_script += smoke_wavelet_noise;

  // Time
  manta_script += header_time + fluid_time_stepping + fluid_adapt_time_step;

  // Import
  manta_script += header_import + fluid_file_import + fluid_cache_helper + fluid_load_data +
                  smoke_load_data;
  if (noise)
    manta_script += smoke_load_noise;
  if (guiding)
    manta_script += fluid_load_guiding;

  // Pre/Post Steps
  manta_script += header_prepost + fluid_pre_step + fluid_post_step;

  // Steps
  manta_script += header_steps + smoke_adaptive_step + smoke_step;
  if (noise) {
    manta_script += smoke_step_noise;
  }

  // Main
  manta_script += header_main + smoke_standalone + fluid_standalone;

  // Fill in missing variables in script
  std::string final_script = MANTA::parseScript(manta_script, mmd);

  // Write script
  std::ofstream myfile;
  myfile.open(cacheDirScript);
  myfile << final_script;
  myfile.close();
}

void MANTA::exportLiquidScript(FluidModifierData *mmd)
{
  if (with_debug)
    std::cout << "MANTA::exportLiquidScript()" << std::endl;

  char cacheDir[FILE_MAX] = "\0";
  char cacheDirScript[FILE_MAX] = "\0";

  BLI_path_join(
      cacheDir, sizeof(cacheDir), mmd->domain->cache_directory, FLUID_DOMAIN_DIR_SCRIPT, nullptr);
  BLI_path_make_safe(cacheDir);
  /* Create 'script' subdir if it does not exist already */
  BLI_dir_create_recursive(cacheDir);
  BLI_path_join(
      cacheDirScript, sizeof(cacheDirScript), cacheDir, FLUID_DOMAIN_LIQUID_SCRIPT, nullptr);
  BLI_path_make_safe(cacheDirScript);

  bool mesh = mmd->domain->flags & FLUID_DOMAIN_USE_MESH;
  bool drops = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY;
  bool bubble = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE;
  bool floater = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM;
  bool tracer = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER;
  bool obstacle = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE;
  bool fractions = mmd->domain->flags & FLUID_DOMAIN_USE_FRACTIONS;
  bool guiding = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_GUIDE;
  bool invel = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL;
  bool outflow = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW;

  std::string manta_script;

  // Libraries
  manta_script += header_libraries + manta_import;

  // Variables
  manta_script += header_variables + fluid_variables + liquid_variables;
  if (mesh)
    manta_script += fluid_variables_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_variables_particles + liquid_variables_particles;
  if (guiding)
    manta_script += fluid_variables_guiding;

  // Solvers
  manta_script += header_solvers + fluid_solver;
  if (mesh)
    manta_script += fluid_solver_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_solver_particles;
  if (guiding)
    manta_script += fluid_solver_guiding;

  // Grids
  manta_script += header_grids + fluid_alloc + liquid_alloc;
  if (mesh)
    manta_script += liquid_alloc_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += liquid_alloc_particles;
  if (guiding)
    manta_script += fluid_alloc_guiding;
  if (obstacle)
    manta_script += fluid_alloc_obstacle;
  if (fractions)
    manta_script += fluid_alloc_fractions;
  if (invel)
    manta_script += fluid_alloc_invel;
  if (outflow)
    manta_script += fluid_alloc_outflow;

  // Domain init
  manta_script += header_gridinit + liquid_init_phi;

  // Time
  manta_script += header_time + fluid_time_stepping + fluid_adapt_time_step;

  // Import
  manta_script += header_import + fluid_file_import + fluid_cache_helper + fluid_load_data +
                  liquid_load_data;
  if (mesh)
    manta_script += liquid_load_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += liquid_load_particles;
  if (guiding)
    manta_script += fluid_load_guiding;

  // Pre/Post Steps
  manta_script += header_prepost + fluid_pre_step + fluid_post_step;

  // Steps
  manta_script += header_steps + liquid_adaptive_step + liquid_step;
  if (mesh)
    manta_script += liquid_step_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += liquid_step_particles;

  // Main
  manta_script += header_main + liquid_standalone + fluid_standalone;

  // Fill in missing variables in script
  std::string final_script = MANTA::parseScript(manta_script, mmd);

  // Write script
  std::ofstream myfile;
  myfile.open(cacheDirScript);
  myfile << final_script;
  myfile.close();
}

/* Call Mantaflow Python functions through this function. Use isAttribute for object attributes,
 * e.g. s.cfl (here 's' is varname, 'cfl' functionName, and isAttribute true) or
 *      grid.getDataPointer (here 's' is varname, 'getDataPointer' functionName, and isAttribute
 * false)
 *
 * Important! Return value: New reference or nullptr
 * Caller of this function needs to handle reference count of returned object. */
static PyObject *callPythonFunction(std::string varName,
                                    std::string functionName,
                                    bool isAttribute = false)
{
  if ((varName == "") || (functionName == "")) {
    if (MANTA::with_debug)
      std::cout << "Missing Python variable name and/or function name -- name is: " << varName
                << ", function name is: " << functionName << std::endl;
    return nullptr;
  }

  PyGILState_STATE gilstate = PyGILState_Ensure();
  PyObject *main = nullptr, *var = nullptr, *func = nullptr, *returnedValue = nullptr;

  /* Be sure to initialise Python before importing main. */
  Py_Initialize();

  // Get pyobject that holds result value
  main = PyImport_ImportModule("__main__");
  if (!main) {
    PyGILState_Release(gilstate);
    return nullptr;
  }

  var = PyObject_GetAttrString(main, varName.c_str());
  if (!var) {
    PyGILState_Release(gilstate);
    return nullptr;
  }

  func = PyObject_GetAttrString(var, functionName.c_str());

  Py_DECREF(var);
  if (!func) {
    PyGILState_Release(gilstate);
    return nullptr;
  }

  if (!isAttribute) {
    returnedValue = PyObject_CallObject(func, nullptr);
    Py_DECREF(func);
  }

  PyGILState_Release(gilstate);
  return (!isAttribute) ? returnedValue : func;
}

/* Argument of this function may be a nullptr.
 * If it's not function will handle the reference count decrement of that argument. */
static void *pyObjectToPointer(PyObject *inputObject)
{
  if (!inputObject)
    return nullptr;

  PyGILState_STATE gilstate = PyGILState_Ensure();

  PyObject *encoded = PyUnicode_AsUTF8String(inputObject);
  char *result = PyBytes_AsString(encoded);

  Py_DECREF(inputObject);

  std::string str(result);
  std::istringstream in(str);
  void *dataPointer = nullptr;
  in >> dataPointer;

  Py_DECREF(encoded);

  PyGILState_Release(gilstate);
  return dataPointer;
}

/* Argument of this function may be a nullptr.
 * If it's not function will handle the reference count decrement of that argument. */
static double pyObjectToDouble(PyObject *inputObject)
{
  if (!inputObject)
    return 0.0;

  PyGILState_STATE gilstate = PyGILState_Ensure();

  /* Cannot use PyFloat_AsDouble() since its error check crashes.
   * Likely because of typedef 'Real' for 'float' types in Mantaflow. */
  double result = PyFloat_AS_DOUBLE(inputObject);
  Py_DECREF(inputObject);

  PyGILState_Release(gilstate);
  return result;
}

/* Argument of this function may be a nullptr.
 * If it's not function will handle the reference count decrement of that argument. */
static long pyObjectToLong(PyObject *inputObject)
{
  if (!inputObject)
    return 0;

  PyGILState_STATE gilstate = PyGILState_Ensure();

  long result = PyLong_AsLong(inputObject);
  Py_DECREF(inputObject);

  PyGILState_Release(gilstate);
  return result;
}

int MANTA::getFrame()
{
  if (with_debug)
    std::cout << "MANTA::getFrame()" << std::endl;

  std::string func = "frame";
  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;

  return pyObjectToLong(callPythonFunction(solver, func, true));
}

float MANTA::getTimestep()
{
  if (with_debug)
    std::cout << "MANTA::getTimestep()" << std::endl;

  std::string func = "timestep";
  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;

  return (float)pyObjectToDouble(callPythonFunction(solver, func, true));
}

bool MANTA::needsRealloc(FluidModifierData *mmd)
{
  FluidDomainSettings *mds = mmd->domain;
  return (mds->res[0] != mResX || mds->res[1] != mResY || mds->res[2] != mResZ);
}

void MANTA::adaptTimestep()
{
  if (with_debug)
    std::cout << "MANTA::adaptTimestep()" << std::endl;

  std::vector<std::string> pythonCommands;
  std::ostringstream ss;

  ss << "fluid_adapt_time_step_" << mCurrentID << "()";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
}

bool MANTA::updateMeshFromFile(std::string filename)
{
  std::string fname(filename);
  std::string::size_type idx;

  idx = fname.rfind('.');
  if (idx != std::string::npos) {
    std::string extension = fname.substr(idx + 1);

    if (extension.compare("gz") == 0)
      return updateMeshFromBobj(filename);
    else if (extension.compare("obj") == 0)
      return updateMeshFromObj(filename);
    else if (extension.compare("uni") == 0)
      return updateMeshFromUni(filename);
    else
      std::cerr << "Fluid Error -- updateMeshFromFile(): Invalid file extension in file: "
                << filename << std::endl;
  }
  else {
    std::cerr << "Fluid Error -- updateMeshFromFile(): Unable to open file: " << filename
              << std::endl;
  }
  return false;
}

bool MANTA::updateMeshFromBobj(std::string filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromBobj()" << std::endl;

  gzFile gzf;

  gzf = (gzFile)BLI_gzopen(filename.c_str(), "rb1");  // do some compression
  if (!gzf) {
    std::cerr << "Fluid Error -- updateMeshFromBobj(): Unable to open file: " << filename
              << std::endl;
    return false;
  }

  int numBuffer = 0, readBytes = 0;

  // Num vertices
  readBytes = gzread(gzf, &numBuffer, sizeof(int));
  if (!readBytes) {
    std::cerr
        << "Fluid Error -- updateMeshFromBobj(): Unable to read number of mesh vertices from "
        << filename << std::endl;
    gzclose(gzf);
    return false;
  }

  if (with_debug)
    std::cout << "read mesh , num verts: " << numBuffer << " , in file: " << filename << std::endl;

  int numChunks = (int)(ceil((float)numBuffer / NODE_CHUNK));
  int readLen, readStart, readEnd, k;

  if (numBuffer) {
    // Vertices
    int todoVertices = numBuffer;
    float *bufferVerts = (float *)MEM_malloc_arrayN(
        NODE_CHUNK, sizeof(float) * 3, "fluid_mesh_vertices");

    mMeshNodes->resize(numBuffer);

    for (int i = 0; i < numChunks && todoVertices > 0; ++i) {
      readLen = NODE_CHUNK;
      if (todoVertices < NODE_CHUNK) {
        readLen = todoVertices;
      }

      readBytes = gzread(gzf, bufferVerts, readLen * sizeof(float) * 3);
      if (!readBytes) {
        std::cerr << "Fluid Error -- updateMeshFromBobj(): Unable to read mesh vertices from "
                  << filename << std::endl;
        MEM_freeN(bufferVerts);
        gzclose(gzf);
        return false;
      }

      readStart = (numBuffer - todoVertices);
      CLAMP(readStart, 0, numBuffer);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numBuffer);

      k = 0;
      for (std::vector<MANTA::Node>::size_type j = readStart; j < readEnd; j++, k += 3) {
        mMeshNodes->at(j).pos[0] = bufferVerts[k];
        mMeshNodes->at(j).pos[1] = bufferVerts[k + 1];
        mMeshNodes->at(j).pos[2] = bufferVerts[k + 2];
      }
      todoVertices -= readLen;
    }
    MEM_freeN(bufferVerts);
  }

  // Num normals
  readBytes = gzread(gzf, &numBuffer, sizeof(int));
  if (!readBytes) {
    std::cerr << "Fluid Error -- updateMeshFromBobj(): Unable to read number of mesh normals from "
              << filename << std::endl;
    gzclose(gzf);
    return false;
  }

  if (with_debug)
    std::cout << "read mesh , num normals : " << numBuffer << " , in file: " << filename
              << std::endl;

  if (numBuffer) {
    // Normals
    int todoNormals = numBuffer;
    float *bufferNormals = (float *)MEM_malloc_arrayN(
        NODE_CHUNK, sizeof(float) * 3, "fluid_mesh_normals");

    if (!getNumVertices())
      mMeshNodes->resize(numBuffer);

    for (int i = 0; i < numChunks && todoNormals > 0; ++i) {
      readLen = NODE_CHUNK;
      if (todoNormals < NODE_CHUNK) {
        readLen = todoNormals;
      }

      readBytes = gzread(gzf, bufferNormals, readLen * sizeof(float) * 3);
      if (!readBytes) {
        std::cerr << "Fluid Error -- updateMeshFromBobj(): Unable to read mesh normals from "
                  << filename << std::endl;
        MEM_freeN(bufferNormals);
        gzclose(gzf);
        return false;
      }

      readStart = (numBuffer - todoNormals);
      CLAMP(readStart, 0, numBuffer);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numBuffer);

      k = 0;
      for (std::vector<MANTA::Node>::size_type j = readStart; j < readEnd; j++, k += 3) {
        mMeshNodes->at(j).normal[0] = bufferNormals[k];
        mMeshNodes->at(j).normal[1] = bufferNormals[k + 1];
        mMeshNodes->at(j).normal[2] = bufferNormals[k + 2];
      }
      todoNormals -= readLen;
    }
    MEM_freeN(bufferNormals);
  }

  // Num triangles
  readBytes = gzread(gzf, &numBuffer, sizeof(int));
  if (!readBytes) {
    std::cerr
        << "Fluid Error -- updateMeshFromBobj(): Unable to read number of mesh triangles from "
        << filename << std::endl;
    gzclose(gzf);
    return false;
  }

  if (with_debug)
    std::cout << "Fluid: Read mesh , num triangles : " << numBuffer << " , in file: " << filename
              << std::endl;

  numChunks = (int)(ceil((float)numBuffer / TRIANGLE_CHUNK));

  if (numBuffer) {
    // Triangles
    int todoTriangles = numBuffer;
    int *bufferTriangles = (int *)MEM_malloc_arrayN(
        TRIANGLE_CHUNK, sizeof(int) * 3, "fluid_mesh_triangles");

    mMeshTriangles->resize(numBuffer);

    for (int i = 0; i < numChunks && todoTriangles > 0; ++i) {
      readLen = TRIANGLE_CHUNK;
      if (todoTriangles < TRIANGLE_CHUNK) {
        readLen = todoTriangles;
      }

      readBytes = gzread(gzf, bufferTriangles, readLen * sizeof(int) * 3);
      if (!readBytes) {
        std::cerr << "Fluid Error -- updateMeshFromBobj(): Unable to read mesh triangles from "
                  << filename << std::endl;
        MEM_freeN(bufferTriangles);
        gzclose(gzf);
        return false;
      }

      readStart = (numBuffer - todoTriangles);
      CLAMP(readStart, 0, numBuffer);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numBuffer);

      k = 0;
      for (std::vector<MANTA::Triangle>::size_type j = readStart; j < readEnd; j++, k += 3) {
        mMeshTriangles->at(j).c[0] = bufferTriangles[k];
        mMeshTriangles->at(j).c[1] = bufferTriangles[k + 1];
        mMeshTriangles->at(j).c[2] = bufferTriangles[k + 2];
      }
      todoTriangles -= readLen;
    }
    MEM_freeN(bufferTriangles);
  }
  return (gzclose(gzf) == Z_OK);
}

bool MANTA::updateMeshFromObj(std::string filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromObj()" << std::endl;

  std::ifstream ifs(filename);
  float fbuffer[3];
  int ibuffer[3];
  int cntVerts = 0, cntNormals = 0, cntTris = 0;

  if (!ifs.good()) {
    std::cerr << "Fluid Error -- updateMeshFromObj(): Unable to open file: " << filename
              << std::endl;
    return false;
  }

  while (ifs.good() && !ifs.eof()) {
    std::string id;
    ifs >> id;

    if (id[0] == '#') {
      // comment
      getline(ifs, id);
      continue;
    }
    if (id == "vt") {
      // tex coord, ignore
    }
    else if (id == "vn") {
      // normals
      if (getNumVertices() != cntVerts) {
        std::cerr << "Fluid Error -- updateMeshFromObj(): Invalid number of mesh nodes in file: "
                  << filename << std::endl;
        return false;
      }

      ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
      MANTA::Node *node = &mMeshNodes->at(cntNormals);
      (*node).normal[0] = fbuffer[0];
      (*node).normal[1] = fbuffer[1];
      (*node).normal[2] = fbuffer[2];
      cntNormals++;
    }
    else if (id == "v") {
      // vertex
      ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
      MANTA::Node node;
      node.pos[0] = fbuffer[0];
      node.pos[1] = fbuffer[1];
      node.pos[2] = fbuffer[2];
      mMeshNodes->push_back(node);
      cntVerts++;
    }
    else if (id == "g") {
      // group
      std::string group;
      ifs >> group;
    }
    else if (id == "f") {
      // face
      std::string face;
      for (int i = 0; i < 3; i++) {
        ifs >> face;
        if (face.find('/') != std::string::npos)
          face = face.substr(0, face.find('/'));  // ignore other indices
        int idx = atoi(face.c_str()) - 1;
        if (idx < 0) {
          std::cerr << "Fluid Error -- updateMeshFromObj(): Invalid face encountered in file: "
                    << filename << std::endl;
          return false;
        }
        ibuffer[i] = idx;
      }
      MANTA::Triangle triangle;
      triangle.c[0] = ibuffer[0];
      triangle.c[1] = ibuffer[1];
      triangle.c[2] = ibuffer[2];
      mMeshTriangles->push_back(triangle);
      cntTris++;
    }
    else {
      // whatever, ignore
    }
    // kill rest of line
    getline(ifs, id);
  }
  ifs.close();
  return true;
}

bool MANTA::updateMeshFromUni(std::string filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromUni()" << std::endl;

  gzFile gzf;
  float fbuffer[4];
  int ibuffer[4];

  gzf = (gzFile)BLI_gzopen(filename.c_str(), "rb1");  // do some compression
  if (!gzf) {
    std::cerr << "Fluid Error -- updateMeshFromUni(): Unable to open file: " << filename
              << std::endl;
    return false;
  }

  int readBytes = 0;
  char file_magic[5] = {0, 0, 0, 0, 0};
  readBytes = gzread(gzf, file_magic, 4);
  if (!readBytes) {
    std::cerr << "Fluid Error -- updateMeshFromUni(): Unable to read header in file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }

  std::vector<pVel> *velocityPointer = mMeshVelocities;

  // mdata uni header
  const int STR_LEN_PDATA = 256;
  int elementType, bytesPerElement, numParticles;
  char info[STR_LEN_PDATA];      // mantaflow build information
  unsigned long long timestamp;  // creation time

  // read mesh header
  gzread(gzf, &ibuffer, sizeof(int) * 4);  // num particles, dimX, dimY, dimZ
  gzread(gzf, &elementType, sizeof(int));
  gzread(gzf, &bytesPerElement, sizeof(int));
  gzread(gzf, &info, sizeof(info));
  gzread(gzf, &timestamp, sizeof(unsigned long long));

  if (with_debug)
    std::cout << "Fluid: Read " << ibuffer[0] << " vertices in file: " << filename << std::endl;

  // Sanity checks
  const int meshSize = sizeof(float) * 3 + sizeof(int);
  if (!(bytesPerElement == meshSize) && (elementType == 0)) {
    std::cerr << "Fluid Error -- updateMeshFromUni(): Invalid header in file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }
  if (!ibuffer[0]) {  // Any vertices present?
    std::cerr << "Fluid Error -- updateMeshFromUni(): No vertices present in file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }

  // Reading mesh
  if (!strcmp(file_magic, "MB01")) {
    // TODO (sebbas): Future update could add uni mesh support
  }
  // Reading mesh data file v1 with vec3
  else if (!strcmp(file_magic, "MD01")) {
    numParticles = ibuffer[0];

    velocityPointer->resize(numParticles);
    MANTA::pVel *bufferPVel;
    for (std::vector<pVel>::iterator it = velocityPointer->begin(); it != velocityPointer->end();
         ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3);
      bufferPVel = (MANTA::pVel *)fbuffer;
      it->pos[0] = bufferPVel->pos[0];
      it->pos[1] = bufferPVel->pos[1];
      it->pos[2] = bufferPVel->pos[2];
    }
  }
  return (gzclose(gzf) == Z_OK);
}

bool MANTA::updateParticlesFromFile(std::string filename, bool isSecondarySys, bool isVelData)
{
  if (with_debug)
    std::cout << "MANTA::updateParticlesFromFile()" << std::endl;

  std::string fname(filename);
  std::string::size_type idx;

  idx = fname.rfind('.');
  if (idx != std::string::npos) {
    std::string extension = fname.substr(idx + 1);

    if (extension.compare("uni") == 0)
      return updateParticlesFromUni(filename, isSecondarySys, isVelData);
    else
      std::cerr << "Fluid Error -- updateParticlesFromFile(): Invalid file extension in file: "
                << filename << std::endl;
    return false;
  }
  else {
    std::cerr << "Fluid Error -- updateParticlesFromFile(): Unable to open file: " << filename
              << std::endl;
    return false;
  }
}

bool MANTA::updateParticlesFromUni(std::string filename, bool isSecondarySys, bool isVelData)
{
  if (with_debug)
    std::cout << "MANTA::updateParticlesFromUni()" << std::endl;

  gzFile gzf;
  int ibuffer[4];

  gzf = (gzFile)BLI_gzopen(filename.c_str(), "rb1");  // do some compression
  if (!gzf) {
    std::cerr << "Fluid Error -- updateParticlesFromUni(): Unable to open file: " << filename
              << std::endl;
    return false;
  }

  int readBytes = 0;
  char file_magic[5] = {0, 0, 0, 0, 0};
  readBytes = gzread(gzf, file_magic, 4);
  if (!readBytes) {
    std::cerr << "Fluid Error -- updateParticlesFromUni(): Unable to read header in file: "
              << filename << std::endl;
    gzclose(gzf);
    return false;
  }

  if (!strcmp(file_magic, "PB01")) {
    std::cerr << "Fluid Error -- updateParticlesFromUni(): Particle uni file format v01 not "
                 "supported anymore."
              << std::endl;
    gzclose(gzf);
    return false;
  }

  // Pointer to FLIP system or to secondary particle system
  std::vector<pData> *dataPointer = nullptr;
  std::vector<pVel> *velocityPointer = nullptr;
  std::vector<float> *lifePointer = nullptr;

  if (isSecondarySys) {
    dataPointer = mSndParticleData;
    velocityPointer = mSndParticleVelocity;
    lifePointer = mSndParticleLife;
  }
  else {
    dataPointer = mFlipParticleData;
    velocityPointer = mFlipParticleVelocity;
  }

  // pdata uni header
  const int STR_LEN_PDATA = 256;
  int elementType, bytesPerElement, numParticles;
  char info[STR_LEN_PDATA];      // mantaflow build information
  unsigned long long timestamp;  // creation time

  // read particle header
  gzread(gzf, &ibuffer, sizeof(int) * 4);  // num particles, dimX, dimY, dimZ
  gzread(gzf, &elementType, sizeof(int));
  gzread(gzf, &bytesPerElement, sizeof(int));
  gzread(gzf, &info, sizeof(info));
  gzread(gzf, &timestamp, sizeof(unsigned long long));

  if (with_debug)
    std::cout << "Fluid: Read " << ibuffer[0] << " particles in file: " << filename << std::endl;

  // Sanity checks
  const int partSysSize = sizeof(float) * 3 + sizeof(int);
  if (!(bytesPerElement == partSysSize) && (elementType == 0)) {
    std::cerr << "Fluid Error -- updateParticlesFromUni(): Invalid header in file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }
  if (!ibuffer[0]) {  // Any particles present?
    if (with_debug)
      std::cout << "Fluid: No particles present in file: " << filename << std::endl;
    gzclose(gzf);
    return true;  // return true since having no particles in a cache file is valid
  }

  numParticles = ibuffer[0];

  const int numChunks = (int)(ceil((float)numParticles / PARTICLE_CHUNK));
  int todoParticles, readLen;
  int readStart, readEnd;

  // Reading base particle system file v2
  if (!strcmp(file_magic, "PB02")) {
    MANTA::pData *bufferPData;
    todoParticles = numParticles;
    bufferPData = (MANTA::pData *)MEM_malloc_arrayN(
        PARTICLE_CHUNK, sizeof(MANTA::pData), "fluid_particle_data");

    dataPointer->resize(numParticles);

    for (int i = 0; i < numChunks && todoParticles > 0; ++i) {
      readLen = PARTICLE_CHUNK;
      if (todoParticles < PARTICLE_CHUNK) {
        readLen = todoParticles;
      }

      readBytes = gzread(gzf, bufferPData, readLen * sizeof(pData));
      if (!readBytes) {
        std::cerr
            << "Fluid Error -- updateParticlesFromUni(): Unable to read particle data in file: "
            << filename << std::endl;
        MEM_freeN(bufferPData);
        gzclose(gzf);
        return false;
      }

      readStart = (numParticles - todoParticles);
      CLAMP(readStart, 0, numParticles);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numParticles);

      int k = 0;
      for (std::vector<MANTA::pData>::size_type j = readStart; j < readEnd; j++, k++) {
        dataPointer->at(j).pos[0] = bufferPData[k].pos[0];
        dataPointer->at(j).pos[1] = bufferPData[k].pos[1];
        dataPointer->at(j).pos[2] = bufferPData[k].pos[2];
        dataPointer->at(j).flag = bufferPData[k].flag;
      }
      todoParticles -= readLen;
    }
    MEM_freeN(bufferPData);
  }
  // Reading particle data file v1 with velocities
  else if (!strcmp(file_magic, "PD01") && isVelData) {
    MANTA::pVel *bufferPVel;
    todoParticles = numParticles;
    bufferPVel = (MANTA::pVel *)MEM_malloc_arrayN(
        PARTICLE_CHUNK, sizeof(MANTA::pVel), "fluid_particle_velocity");

    velocityPointer->resize(numParticles);

    for (int i = 0; i < numChunks && todoParticles > 0; ++i) {
      readLen = PARTICLE_CHUNK;
      if (todoParticles < PARTICLE_CHUNK) {
        readLen = todoParticles;
      }

      readBytes = gzread(gzf, bufferPVel, readLen * sizeof(pVel));
      if (!readBytes) {
        std::cerr << "Fluid Error -- updateParticlesFromUni(): Unable to read particle velocities "
                     "in file: "
                  << filename << std::endl;
        MEM_freeN(bufferPVel);
        gzclose(gzf);
        return false;
      }

      readStart = (numParticles - todoParticles);
      CLAMP(readStart, 0, numParticles);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numParticles);

      int k = 0;
      for (std::vector<MANTA::pVel>::size_type j = readStart; j < readEnd; j++, k++) {
        velocityPointer->at(j).pos[0] = bufferPVel[k].pos[0];
        velocityPointer->at(j).pos[1] = bufferPVel[k].pos[1];
        velocityPointer->at(j).pos[2] = bufferPVel[k].pos[2];
      }
      todoParticles -= readLen;
    }
    MEM_freeN(bufferPVel);
  }
  // Reading particle data file v1 with lifetime
  else if (!strcmp(file_magic, "PD01")) {
    float *bufferPLife;
    todoParticles = numParticles;
    bufferPLife = (float *)MEM_malloc_arrayN(PARTICLE_CHUNK, sizeof(float), "fluid_particle_life");

    lifePointer->resize(numParticles);

    for (int i = 0; i < numChunks && todoParticles > 0; ++i) {
      readLen = PARTICLE_CHUNK;
      if (todoParticles < PARTICLE_CHUNK) {
        readLen = todoParticles;
      }

      readBytes = gzread(gzf, bufferPLife, readLen * sizeof(float));
      if (!readBytes) {
        std::cerr
            << "Fluid Error -- updateParticlesFromUni(): Unable to read particle life in file: "
            << filename << std::endl;
        MEM_freeN(bufferPLife);
        gzclose(gzf);
        return false;
      }

      readStart = (numParticles - todoParticles);
      CLAMP(readStart, 0, numParticles);
      readEnd = readStart + readLen;
      CLAMP(readEnd, 0, numParticles);

      int k = 0;
      for (std::vector<float>::size_type j = readStart; j < readEnd; j++, k++) {
        lifePointer->at(j) = bufferPLife[k];
      }
      todoParticles -= readLen;
    }
    MEM_freeN(bufferPLife);
  }
  return (gzclose(gzf) == Z_OK);
}

bool MANTA::updateGridFromFile(std::string filename, float *grid, bool isNoise)
{
  if (with_debug)
    std::cout << "MANTA::updateGridFromFile()" << std::endl;

  if (!grid) {
    std::cerr << "Fluid Error -- updateGridFromFile(): Cannot read into uninitialized grid (grid "
                 "is null)."
              << std::endl;
    return false;
  }

  std::string fname(filename);
  std::string::size_type idx;

  idx = fname.rfind('.');
  if (idx != std::string::npos) {
    std::string extension = fname.substr(idx + 1);

    if (extension.compare("uni") == 0)
      return updateGridFromUni(filename, grid, isNoise);
#if OPENVDB == 1
    else if (extension.compare("vdb") == 0)
      return updateGridFromVDB(filename, grid, isNoise);
#endif
    else if (extension.compare("raw") == 0)
      return updateGridFromRaw(filename, grid, isNoise);
    else
      std::cerr << "Fluid Error -- updateGridFromFile(): Invalid file extension in file: "
                << filename << std::endl;
    return false;
  }
  else {
    std::cerr << "Fluid Error -- updateGridFromFile(): Unable to open file: " << filename
              << std::endl;
    return false;
  }
}

bool MANTA::updateGridFromUni(std::string filename, float *grid, bool isNoise)
{
  if (with_debug)
    std::cout << "MANTA::updateGridFromUni()" << std::endl;

  gzFile gzf;
  int ibuffer[4];

  gzf = (gzFile)BLI_gzopen(filename.c_str(), "rb1");
  if (!gzf) {
    std::cerr << "Fluid Error -- updateGridFromUni(): Unable to open file: " << filename
              << std::endl;
    return false;
  }

  int readBytes = 0;
  char file_magic[5] = {0, 0, 0, 0, 0};
  readBytes = gzread(gzf, file_magic, 4);
  if (!readBytes) {
    std::cerr << "Fluid Error -- updateGridFromUni(): Unable to read header in file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }

  if (!strcmp(file_magic, "DDF2")) {
    std::cerr
        << "Fluid Error -- updateGridFromUni(): Grid uni file format DDF2 not supported anymore."
        << std::endl;
    gzclose(gzf);
    return false;
  }

  if (!strcmp(file_magic, "MNT1")) {
    std::cerr
        << "Fluid Error -- updateGridFromUni(): Grid uni file format MNT1 not supported anymore."
        << std::endl;
    gzclose(gzf);
    return false;
  }

  if (!strcmp(file_magic, "MNT2")) {
    std::cerr
        << "Fluid Error -- updateGridFromUni(): Grid uni file format MNT2 not supported anymore."
        << std::endl;
    gzclose(gzf);
    return false;
  }

  // grid uni header
  const int STR_LEN_GRID = 252;
  int elementType, bytesPerElement;  // data type info
  char info[STR_LEN_GRID];           // mantaflow build information
  int dimT;                          // optionally store forth dimension for 4d grids
  unsigned long long timestamp;      // creation time

  // read grid header
  gzread(gzf, &ibuffer, sizeof(int) * 4);  // dimX, dimY, dimZ, gridType
  gzread(gzf, &elementType, sizeof(int));
  gzread(gzf, &bytesPerElement, sizeof(int));
  gzread(gzf, &info, sizeof(info));
  gzread(gzf, &dimT, sizeof(int));
  gzread(gzf, &timestamp, sizeof(unsigned long long));

  int resX = (isNoise) ? mResXNoise : mResX;
  int resY = (isNoise) ? mResYNoise : mResY;
  int resZ = (isNoise) ? mResZNoise : mResZ;

  if (with_debug)
    std::cout << "Fluid: Read " << ibuffer[3] << " grid type in file: " << filename << std::endl;

  // Sanity checks
  if (ibuffer[0] != resX || ibuffer[1] != resY || ibuffer[2] != resZ) {
    std::cout << "Fluid: Grid dim doesn't match, read: (" << ibuffer[0] << ", " << ibuffer[1]
              << ", " << ibuffer[2] << ") vs setup: (" << resX << ", " << resY << ", " << resZ
              << ")" << std::endl;
    gzclose(gzf);
    return false;
  }

  // Actual data reading
  if (!strcmp(file_magic, "MNT3")) {
    gzread(gzf, grid, sizeof(float) * ibuffer[0] * ibuffer[1] * ibuffer[2]);
  }

  if (with_debug)
    std::cout << "Fluid: Read successfully: " << filename << std::endl;

  return (gzclose(gzf) == Z_OK);
}

#if OPENVDB == 1
bool MANTA::updateGridFromVDB(std::string filename, float *grid, bool isNoise)
{
  if (with_debug)
    std::cout << "MANTA::updateGridFromVDB()" << std::endl;

  openvdb::initialize();
  openvdb::io::File file(filename);
  try {
    file.open();
  }
  catch (const openvdb::IoError &) {
    std::cerr << "Fluid Error -- updateGridFromVDB(): IOError, invalid OpenVDB file: " << filename
              << std::endl;
    return false;
  }

  openvdb::GridBase::Ptr baseGrid;
  for (openvdb::io::File::NameIterator nameIter = file.beginName(); nameIter != file.endName();
       ++nameIter) {
    baseGrid = file.readGrid(nameIter.gridName());
    break;
  }
  file.close();
  openvdb::FloatGrid::Ptr gridVDB = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
  openvdb::FloatGrid::Accessor accessor = gridVDB->getAccessor();

  int resX = (isNoise) ? mResXNoise : mResX;
  int resY = (isNoise) ? mResYNoise : mResY;
  int resZ = (isNoise) ? mResZNoise : mResZ;

  size_t index = 0;
  for (int z = 0; z < resZ; ++z) {
    for (int y = 0; y < resY; ++y) {
      for (int x = 0; x < resX; ++x, ++index) {
        openvdb::Coord xyz(x, y, z);
        float v = accessor.getValue(xyz);
        grid[index] = v;
      }
    }
  }
  return true;
}
#endif

bool MANTA::updateGridFromRaw(std::string filename, float *grid, bool isNoise)
{
  if (with_debug)
    std::cout << "MANTA::updateGridFromRaw()" << std::endl;

  gzFile gzf;
  int expectedBytes, readBytes;

  gzf = (gzFile)BLI_gzopen(filename.c_str(), "rb");
  if (!gzf) {
    std::cout << "MANTA::updateGridFromRaw(): unable to open file" << std::endl;
    return false;
  }

  int resX = (isNoise) ? mResXNoise : mResX;
  int resY = (isNoise) ? mResYNoise : mResY;
  int resZ = (isNoise) ? mResZNoise : mResZ;

  expectedBytes = sizeof(float) * resX * resY * resZ;
  readBytes = gzread(gzf, grid, expectedBytes);
  if (!readBytes) {
    std::cerr << "Fluid Error -- updateGridFromRaw(): Unable to read raw file: " << filename
              << std::endl;
    gzclose(gzf);
    return false;
  }

  assert(expectedBytes == readBytes);

  return (gzclose(gzf) == Z_OK);
}

void MANTA::updatePointers()
{
  if (with_debug)
    std::cout << "MANTA::updatePointers()" << std::endl;

  std::string func = "getDataPointer";
  std::string funcNodes = "getNodesDataPointer";
  std::string funcTris = "getTrisDataPointer";

  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;
  std::string parts = "pp" + id;
  std::string snd = "sp" + id;
  std::string mesh = "sm" + id;
  std::string mesh2 = "mesh" + id;
  std::string noise = "sn" + id;
  std::string solver_ext = "_" + solver;
  std::string parts_ext = "_" + parts;
  std::string snd_ext = "_" + snd;
  std::string mesh_ext = "_" + mesh;
  std::string mesh_ext2 = "_" + mesh2;
  std::string noise_ext = "_" + noise;

  mFlags = (int *)pyObjectToPointer(callPythonFunction("flags" + solver_ext, func));
  mPhiIn = (float *)pyObjectToPointer(callPythonFunction("phiIn" + solver_ext, func));
  mPhiStaticIn = (float *)pyObjectToPointer(callPythonFunction("phiSIn" + solver_ext, func));
  mVelocityX = (float *)pyObjectToPointer(callPythonFunction("x_vel" + solver_ext, func));
  mVelocityY = (float *)pyObjectToPointer(callPythonFunction("y_vel" + solver_ext, func));
  mVelocityZ = (float *)pyObjectToPointer(callPythonFunction("z_vel" + solver_ext, func));
  mForceX = (float *)pyObjectToPointer(callPythonFunction("x_force" + solver_ext, func));
  mForceY = (float *)pyObjectToPointer(callPythonFunction("y_force" + solver_ext, func));
  mForceZ = (float *)pyObjectToPointer(callPythonFunction("z_force" + solver_ext, func));

  if (mUsingOutflow) {
    mPhiOutIn = (float *)pyObjectToPointer(callPythonFunction("phiOutIn" + solver_ext, func));
    mPhiOutStaticIn = (float *)pyObjectToPointer(
        callPythonFunction("phiOutSIn" + solver_ext, func));
  }
  if (mUsingObstacle) {
    mPhiObsIn = (float *)pyObjectToPointer(callPythonFunction("phiObsIn" + solver_ext, func));
    mPhiObsStaticIn = (float *)pyObjectToPointer(
        callPythonFunction("phiObsSIn" + solver_ext, func));
    mObVelocityX = (float *)pyObjectToPointer(callPythonFunction("x_obvel" + solver_ext, func));
    mObVelocityY = (float *)pyObjectToPointer(callPythonFunction("y_obvel" + solver_ext, func));
    mObVelocityZ = (float *)pyObjectToPointer(callPythonFunction("z_obvel" + solver_ext, func));
    mNumObstacle = (float *)pyObjectToPointer(callPythonFunction("numObs" + solver_ext, func));
  }
  if (mUsingGuiding) {
    mPhiGuideIn = (float *)pyObjectToPointer(callPythonFunction("phiGuideIn" + solver_ext, func));
    mGuideVelocityX = (float *)pyObjectToPointer(
        callPythonFunction("x_guidevel" + solver_ext, func));
    mGuideVelocityY = (float *)pyObjectToPointer(
        callPythonFunction("y_guidevel" + solver_ext, func));
    mGuideVelocityZ = (float *)pyObjectToPointer(
        callPythonFunction("z_guidevel" + solver_ext, func));
    mNumGuide = (float *)pyObjectToPointer(callPythonFunction("numGuides" + solver_ext, func));
  }
  if (mUsingInvel) {
    mInVelocityX = (float *)pyObjectToPointer(callPythonFunction("x_invel" + solver_ext, func));
    mInVelocityY = (float *)pyObjectToPointer(callPythonFunction("y_invel" + solver_ext, func));
    mInVelocityZ = (float *)pyObjectToPointer(callPythonFunction("z_invel" + solver_ext, func));
  }
  if (mUsingSmoke) {
    mDensity = (float *)pyObjectToPointer(callPythonFunction("density" + solver_ext, func));
    mDensityIn = (float *)pyObjectToPointer(callPythonFunction("densityIn" + solver_ext, func));
    mShadow = (float *)pyObjectToPointer(callPythonFunction("shadow" + solver_ext, func));
    mEmissionIn = (float *)pyObjectToPointer(callPythonFunction("emissionIn" + solver_ext, func));
  }
  if (mUsingSmoke && mUsingHeat) {
    mHeat = (float *)pyObjectToPointer(callPythonFunction("heat" + solver_ext, func));
    mHeatIn = (float *)pyObjectToPointer(callPythonFunction("heatIn" + solver_ext, func));
  }
  if (mUsingSmoke && mUsingFire) {
    mFlame = (float *)pyObjectToPointer(callPythonFunction("flame" + solver_ext, func));
    mFuel = (float *)pyObjectToPointer(callPythonFunction("fuel" + solver_ext, func));
    mReact = (float *)pyObjectToPointer(callPythonFunction("react" + solver_ext, func));
    mFuelIn = (float *)pyObjectToPointer(callPythonFunction("fuelIn" + solver_ext, func));
    mReactIn = (float *)pyObjectToPointer(callPythonFunction("reactIn" + solver_ext, func));
  }
  if (mUsingSmoke && mUsingColors) {
    mColorR = (float *)pyObjectToPointer(callPythonFunction("color_r" + solver_ext, func));
    mColorG = (float *)pyObjectToPointer(callPythonFunction("color_g" + solver_ext, func));
    mColorB = (float *)pyObjectToPointer(callPythonFunction("color_b" + solver_ext, func));
    mColorRIn = (float *)pyObjectToPointer(callPythonFunction("color_r_in" + solver_ext, func));
    mColorGIn = (float *)pyObjectToPointer(callPythonFunction("color_g_in" + solver_ext, func));
    mColorBIn = (float *)pyObjectToPointer(callPythonFunction("color_b_in" + solver_ext, func));
  }
  if (mUsingSmoke && mUsingNoise) {
    mDensityHigh = (float *)pyObjectToPointer(callPythonFunction("density" + noise_ext, func));
    mTextureU = (float *)pyObjectToPointer(callPythonFunction("texture_u" + solver_ext, func));
    mTextureV = (float *)pyObjectToPointer(callPythonFunction("texture_v" + solver_ext, func));
    mTextureW = (float *)pyObjectToPointer(callPythonFunction("texture_w" + solver_ext, func));
    mTextureU2 = (float *)pyObjectToPointer(callPythonFunction("texture_u2" + solver_ext, func));
    mTextureV2 = (float *)pyObjectToPointer(callPythonFunction("texture_v2" + solver_ext, func));
    mTextureW2 = (float *)pyObjectToPointer(callPythonFunction("texture_w2" + solver_ext, func));
  }
  if (mUsingSmoke && mUsingNoise && mUsingFire) {
    mFlameHigh = (float *)pyObjectToPointer(callPythonFunction("flame" + noise_ext, func));
    mFuelHigh = (float *)pyObjectToPointer(callPythonFunction("fuel" + noise_ext, func));
    mReactHigh = (float *)pyObjectToPointer(callPythonFunction("react" + noise_ext, func));
  }
  if (mUsingSmoke && mUsingNoise && mUsingColors) {
    mColorRHigh = (float *)pyObjectToPointer(callPythonFunction("color_r" + noise_ext, func));
    mColorGHigh = (float *)pyObjectToPointer(callPythonFunction("color_g" + noise_ext, func));
    mColorBHigh = (float *)pyObjectToPointer(callPythonFunction("color_b" + noise_ext, func));
  }
  if (mUsingLiquid) {
    mPhi = (float *)pyObjectToPointer(callPythonFunction("phi" + solver_ext, func));
    mFlipParticleData = (std::vector<pData> *)pyObjectToPointer(
        callPythonFunction("pp" + solver_ext, func));
    mFlipParticleVelocity = (std::vector<pVel> *)pyObjectToPointer(
        callPythonFunction("pVel" + parts_ext, func));
  }
  if (mUsingLiquid && mUsingMesh) {
    mMeshNodes = (std::vector<Node> *)pyObjectToPointer(
        callPythonFunction("mesh" + mesh_ext, funcNodes));
    mMeshTriangles = (std::vector<Triangle> *)pyObjectToPointer(
        callPythonFunction("mesh" + mesh_ext, funcTris));
  }
  if (mUsingLiquid && mUsingMVel) {
    mMeshVelocities = (std::vector<pVel> *)pyObjectToPointer(
        callPythonFunction("mVel" + mesh_ext2, func));
  }
  if (mUsingLiquid && (mUsingDrops | mUsingBubbles | mUsingFloats | mUsingTracers)) {
    mSndParticleData = (std::vector<pData> *)pyObjectToPointer(
        callPythonFunction("ppSnd" + snd_ext, func));
    mSndParticleVelocity = (std::vector<pVel> *)pyObjectToPointer(
        callPythonFunction("pVelSnd" + parts_ext, func));
    mSndParticleLife = (std::vector<float> *)pyObjectToPointer(
        callPythonFunction("pLifeSnd" + parts_ext, func));
  }

  mFlipFromFile = false;
  mMeshFromFile = false;
  mParticlesFromFile = false;
  mSmokeFromFile = false;
  mNoiseFromFile = false;
}

bool MANTA::hasConfig(FluidModifierData *mmd, int framenr)
{
  std::string extension = FLUID_DOMAIN_EXTENSION_UNI;
  return BLI_exists(
      getFile(mmd, FLUID_DOMAIN_DIR_CONFIG, FLUID_DOMAIN_FILE_CONFIG, extension, framenr).c_str());
}

bool MANTA::hasData(FluidModifierData *mmd, int framenr)
{
  std::string filename = (mUsingSmoke) ? FLUID_DOMAIN_FILE_DENSITY : FLUID_DOMAIN_FILE_PP;
  std::string extension = getCacheFileEnding(mmd->domain->cache_data_format);
  return BLI_exists(getFile(mmd, FLUID_DOMAIN_DIR_DATA, filename, extension, framenr).c_str());
}

bool MANTA::hasNoise(FluidModifierData *mmd, int framenr)
{
  std::string extension = getCacheFileEnding(mmd->domain->cache_noise_format);
  return BLI_exists(
      getFile(mmd, FLUID_DOMAIN_DIR_NOISE, FLUID_DOMAIN_FILE_DENSITYNOISE, extension, framenr)
          .c_str());
}

bool MANTA::hasMesh(FluidModifierData *mmd, int framenr)
{
  std::string extension = getCacheFileEnding(mmd->domain->cache_mesh_format);
  return BLI_exists(
      getFile(mmd, FLUID_DOMAIN_DIR_MESH, FLUID_DOMAIN_FILE_MESH, extension, framenr).c_str());
}

bool MANTA::hasParticles(FluidModifierData *mmd, int framenr)
{
  std::string extension = getCacheFileEnding(mmd->domain->cache_particle_format);
  return BLI_exists(
      getFile(mmd, FLUID_DOMAIN_DIR_PARTICLES, FLUID_DOMAIN_FILE_PPSND, extension, framenr)
          .c_str());
}

bool MANTA::hasGuiding(FluidModifierData *mmd, int framenr, bool sourceDomain)
{
  std::string subdirectory = (sourceDomain) ? FLUID_DOMAIN_DIR_DATA : FLUID_DOMAIN_DIR_GUIDE;
  std::string filename = (sourceDomain) ? FLUID_DOMAIN_FILE_VEL : FLUID_DOMAIN_FILE_GUIDEVEL;
  std::string extension = getCacheFileEnding(mmd->domain->cache_data_format);
  return BLI_exists(getFile(mmd, subdirectory, filename, extension, framenr).c_str());
}

std::string MANTA::getDirectory(FluidModifierData *mmd, std::string subdirectory)
{
  char directory[FILE_MAX];
  BLI_path_join(
      directory, sizeof(directory), mmd->domain->cache_directory, subdirectory.c_str(), nullptr);
  BLI_path_make_safe(directory);
  return directory;
}

std::string MANTA::getFile(FluidModifierData *mmd,
                           std::string subdirectory,
                           std::string fname,
                           std::string extension,
                           int framenr)
{
  char targetFile[FILE_MAX];
  std::string path = getDirectory(mmd, subdirectory);
  std::string filename = fname + extension;
  BLI_join_dirfile(targetFile, sizeof(targetFile), path.c_str(), filename.c_str());
  BLI_path_frame(targetFile, framenr, 0);
  return targetFile;
}

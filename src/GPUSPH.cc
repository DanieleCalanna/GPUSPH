/*  Copyright 2011-2013 Alexis Herault, Giuseppe Bilotta, Robert A. Dalrymple, Eugenio Rustico, Ciro Del Negro

    Istituto Nazionale di Geofisica e Vulcanologia
        Sezione di Catania, Catania, Italy

    Università di Catania, Catania, Italy

    Johns Hopkins University, Baltimore, MD

    This file is part of GPUSPH.

    GPUSPH is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GPUSPH is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GPUSPH.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <float.h> // FLT_EPSILON

#define GPUSPH_MAIN
#include "particledefine.h"
#undef GPUSPH_MAIN

// NOTE: including GPUSPH.h before particledefine.h does not compile.
// This inclusion problem should be solved
#include "GPUSPH.h"

// Writer types
#include "TextWriter.h"
#include "VTKWriter.h"
#include "VTKLegacyWriter.h"
#include "CustomTextWriter.h"
#include "UDPWriter.h"

/* Include only the problem selected at compile time */
#include "problem_select.opt"

/* Include all other opt file for show_version */
#include "gpusph_version.opt"
#include "fastmath_select.opt"
#include "dbg_select.opt"
#include "compute_select.opt"

using namespace std;

GPUSPH* GPUSPH::getInstance() {
	// guaranteed to be destroyed; instantiated on first use
	static GPUSPH instance;
	// return a reference, not just a pointer
	return &instance;
}

GPUSPH::GPUSPH() {
	clOptions = NULL;
	gdata = NULL;
	problem = NULL;
	initialized = false;
}

GPUSPH::~GPUSPH() {
	// it would be useful to have a "fallback" deallocation but we have to check
	// that main did not do that already
	if (initialized) finalize();
}

// used to pretty-print memory amounts. TODO refactor
static const char *memSuffix[] = {
	"B", "KiB", "MiB", "GiB", "TiB"
};
static const size_t memSuffix_els = sizeof(memSuffix)/sizeof(*memSuffix);

bool GPUSPH::initialize(GlobalData *_gdata) {

	printf("Initializing...\n");

	gdata = _gdata;
	clOptions = gdata->clOptions;
	problem = gdata->problem;

	// run post-construction functions
	problem->check_dt();
	problem->check_maxneibsnum();
	problem->create_problem_dir();

	printf("Problem calling set grid params\n");
	problem->set_grid_params();

	m_performanceCounter = new IPPSCounter();

	// utility pointer
	SimParams *_sp = gdata->problem->get_simparams();

	// copy the options passed by command line to GlobalData
	gdata->nosave = clOptions->nosave;
	if (isfinite(clOptions->tend))
		_sp-> tend = clOptions->tend;

	// update the GlobalData copies of the sizes of the domain
	gdata->worldOrigin = make_float3(problem->get_worldorigin());
	gdata->worldSize = make_float3(problem->get_worldsize());
	// TODO: re-enable the followin after the WriterType rampage is over
	// gdata->writerType = problem->get_writertype();

	// get the grid size
	gdata->gridSize = problem->get_gridsize();

	// compute the number of cells
	gdata->nGridCells = gdata->gridSize.x * gdata->gridSize.y * gdata->gridSize.z;

	// get the cell size
	gdata->cellSize = make_float3(problem->get_cellsize());

	printf(" - World origin: %g , %g , %g\n", gdata->worldOrigin.x, gdata->worldOrigin.y, gdata->worldOrigin.z);
	printf(" - World size:   %g x %g x %g\n", gdata->worldSize.x, gdata->worldSize.y, gdata->worldSize.z);
	printf(" - Cell size:    %g x %g x %g\n", gdata->cellSize.x, gdata->cellSize.y, gdata->cellSize.z);
	printf(" - Grid size:    %u x %u x %u (%s cells)\n", gdata->gridSize.x, gdata->gridSize.y, gdata->gridSize.z, gdata->addSeparators(gdata->nGridCells).c_str());


	// initial dt (or, just dt in case adaptive is enabled)
	gdata->dt = _sp->dt;

	// the initial assignment is arbitrary, just need to be complementary
	// (caveat: as long as these pointers, and not just 0 and 1 values, are always used)
	gdata->currentPosRead = gdata->currentVelRead = gdata->currentInfoRead =
		gdata->currentBoundElementRead = gdata->currentGradGammaRead = gdata->currentVerticesRead =
		gdata->currentPressureRead = gdata->currentTKERead = gdata->currentEpsRead = gdata->currentTurbViscRead =
		gdata->currentStrainRateRead = 0;
	gdata->currentPosWrite = gdata->currentVelWrite = gdata->currentInfoWrite =
		gdata->currentBoundElementWrite = gdata->currentGradGammaWrite = gdata->currentVerticesWrite =
		gdata->currentPressureWrite = gdata->currentTKEWrite = gdata->currentEpsWrite = gdata->currentTurbViscWrite =
		gdata->currentStrainRateWrite = 1;

	// check the number of moving boundaries
	if (problem->m_mbnumber > MAXMOVINGBOUND) {
		printf("FATAL: unsupported number of moving boundaries (%u > %u)\n", problem->m_mbnumber, MAXMOVINGBOUND);
		return false;
	}

	// compute mbdata size
	gdata->mbDataSize = problem->m_mbnumber * sizeof(float4);

	// sets the correct viscosity coefficient according to the one set in SimParams
	setViscosityCoefficient();

	// create the Writer according to the WriterType
	createWriter();

	// TODO: writeSummary

	// we should need no PS anymore
	//		> new PS
	// psystem = new ParticleSystem(gdata);
	//			PS constructor updates cellSize, worldSize, nCells, etc. in gdata
	//			PS creates new writer. Do it outside?

	// no: done
	//		> new Problem

	printf("Generating problem particles...\n");
	// allocate the particles of the *whole* simulation
	gdata->totParticles = problem->fill_parts();

	// generate planes, will be allocated in allocateGlobalHostBuffers()
	gdata->numPlanes = problem->fill_planes();

	// initialize CGs (or, the problem could directly write on gdata)
	initializeObjectsCGs();

	// allocate aux arrays for rollCallParticles()
	m_rcBitmap = (bool*) calloc( sizeof(bool) , gdata->totParticles );
	m_rcNotified = (bool*) calloc( sizeof(bool) , gdata->totParticles );
	m_rcAddrs = (uint*) calloc( sizeof(uint) , gdata->totParticles );

	printf("Allocating shared host buffers...\n");
	// allocate cpu buffers, 1 per process
	size_t totCPUbytes = allocateGlobalHostBuffers();

	// pretty print
	printf("  allocated %s on host for %s particles\n",
		gdata->memString(totCPUbytes).c_str(),
		gdata->addSeparators(gdata->processParticles[gdata->mpi_rank]).c_str());

	// copy planes from the problem to the shared array
	problem->copy_planes(gdata->s_hPlanes, gdata->s_hPlanesDiv);

	// let the Problem partition the domain (with global device ids)
	// NOTE: this could be done before fill_parts(), as long as it does not need knowledge about the fluid, but
	// not before allocating the host buffers
	if (MULTI_DEVICE) {
		printf("Splitting the domain in %u partitions...\n", gdata->totDevices);
		// fill the device map with numbers from 0 to totDevices
		gdata->problem->fillDeviceMap(gdata);
		// here it is possible to save the device map before the conversion
		// gdata->saveDeviceMapToFile("linearIdx");
		if (MULTI_NODE) {
			// make the numbers globalDeviceIndices, with the least 3 bits reserved for the device number
			gdata->convertDeviceMap();
			// here it is possible to save the converted device map
			// gdata->saveDeviceMapToFile("");
		}
	}

	printf("Copying the particles to shared arrays...\n");
	printf("---\n");
	// Check: is this mandatory? this requires double the memory!
	// copy particles from problem to GPUSPH buffers
	if (_sp->boundarytype == MF_BOUNDARY) {
		problem->copy_to_array(gdata->s_hPos, gdata->s_hVel, gdata->s_hInfo, gdata->s_hVertices, gdata->s_hBoundElement, gdata->s_hParticleHash);
	} else {
		problem->copy_to_array(gdata->s_hPos, gdata->s_hVel, gdata->s_hInfo, gdata->s_hParticleHash);
	}
	printf("---\n");

	// initialize values of k and e for k-e model
	if (_sp->visctype == KEPSVISC)
		problem->init_keps(gdata->s_hTKE, gdata->s_hEps, gdata->totParticles, gdata->s_hInfo);

	if (MULTI_DEVICE) {
		printf("Sorting the particles per device...\n");
		sortParticlesByHash();
	} else {
		// if there is something more to do, encapsulate in a dedicated method please
		gdata->s_hStartPerDevice[0] = 0;
		gdata->s_hPartsPerDevice[0] = gdata->processParticles[0] =
				gdata->totParticles;
	}

	for (uint d=0; d < gdata->devices; d++)
		printf(" - device at index %u has %s particles assigned and offset %s\n",
			d, gdata->addSeparators(gdata->s_hPartsPerDevice[d]).c_str(), gdata->addSeparators(gdata->s_hStartPerDevice[d]).c_str());

	// TODO
	//		// > new Integrator

	// TODO: read DEM file. setDemTexture() will be called from the GPUWokers instead

	// new Synchronizer; it will be waiting on #devices+1 threads (GPUWorkers + main)
	gdata->threadSynchronizer = new Synchronizer(gdata->devices + 1);

	printf("Starting workers...\n");

	// allocate workers
	gdata->GPUWORKERS = (GPUWorker**)calloc(gdata->devices, sizeof(GPUWorker*));
	for (int d=0; d < gdata->devices; d++)
		gdata->GPUWORKERS[d] = new GPUWorker(gdata, d);

	gdata->keep_going = true;

	// actually start the threads
	for (int d = 0; d < gdata->devices; d++)
		gdata->GPUWORKERS[d]->run_worker(); // begin of INITIALIZATION ***

	// The following barrier waits for GPUworkers to complete CUDA init, GPU allocation, subdomain and devmap upload

	gdata->threadSynchronizer->barrier(); // end of INITIALIZATION ***

	return (initialized = true);
}

bool GPUSPH::finalize() {
	// TODO here, when there will be the Integrator
	// delete Integrator

	printf("Deallocating...\n");

	// suff for rollCallParticles()
	free(m_rcBitmap);
	free(m_rcNotified);
	free(m_rcAddrs);

	// workers
	for (int d = 0; d < gdata->devices; d++)
		delete gdata->GPUWORKERS[d];

	delete gdata->GPUWORKERS;

	// Synchronizer
	delete gdata->threadSynchronizer;

	// host buffers
	deallocateGlobalHostBuffers();

	// ParticleSystem which, in turn, deletes the Writer
	//delete psystem;

	delete gdata->writer;

	// ...anything else?

	delete m_performanceCounter;

	initialized = false;

	return true;
}

bool GPUSPH::runSimulation() {
	if (!initialized) return false;

	// doing first write
	printf("Performing first write...\n");
	doWrite();

	printf("Letting threads upload the subdomains...\n");
	gdata->threadSynchronizer->barrier(); // begins UPLOAD ***

	// here the Workers are uploading their subdomains

	// After next barrier, the workers will enter their simulation cycle, so it is recommended to set
	// nextCommand properly before the barrier (although should be already initialized to IDLE).
	// doCommand(IDLE) would be equivalent, but this is more clear
	gdata->nextCommand = IDLE;
	gdata->threadSynchronizer->barrier(); // end of UPLOAD, begins SIMULATION ***
	gdata->threadSynchronizer->barrier(); // unlock CYCLE BARRIER 1

	// this is where we invoke initialization routines that have to be
	// run by the GPUWokers

	if (problem->get_simparams()->boundarytype == MF_BOUNDARY) {
		initializeGammaAndGradGamma();
		imposeDynamicBoundaryConditions();
		updateValuesAtBoundaryElements();
	}

	printf("Entering the main simulation cycle\n");

	//  IPPS counter does not take the initial uploads into consideration
	m_performanceCounter->start();

	// write some info. This could replace "Entering the main simulation cycle"
	printStatus();

	while (gdata->keep_going) {
		// when there will be an Integrator class, here (or after bneibs?) we will
		// call Integrator -> setNextStep

		// build neighbors list
		if (gdata->iterations % problem->get_simparams()->buildneibsfreq == 0) {
			buildNeibList();
		}

		uint shepardfreq = problem->get_simparams()->shepardfreq;
		if (shepardfreq > 0 && gdata->iterations > 0 && (gdata->iterations % shepardfreq == 0)) {
			gdata->only_internal = true;
			doCommand(SHEPARD);
			// update before swapping, since UPDATE_EXTERNAL works on write buffers
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | DBLBUFFER_WRITE);
			gdata->swapDeviceBuffers(BUFFER_VEL);
		}

		uint mlsfreq = problem->get_simparams()->mlsfreq;
		if (mlsfreq > 0 && gdata->iterations > 0 && (gdata->iterations % mlsfreq == 0)) {
			gdata->only_internal = true;
			doCommand(MLS);
			// update before swapping, since UPDATE_EXTERNAL works on write buffers
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | DBLBUFFER_WRITE);
			gdata->swapDeviceBuffers(BUFFER_VEL);
		}

		//			//(init bodies)

		// moving boundaries
		if (problem->get_simparams()->mbcallback) {
			// ask the Problem to update mbData, one per process
			gdata->commandFlags = INTEGRATOR_STEP_1;
			doCallBacks();
			// upload on the GPU, one per device
			doCommand(UPLOAD_MBDATA);
		}

		// variable gravity
		if (problem->get_simparams()->gcallback) {
			// ask the Problem to update gravity, one per process
			doCallBacks();
			// upload on the GPU, one per device
			doCommand(UPLOAD_GRAVITY);
		}

		// semi-analytical boundary update
		if (problem->get_simparams()->boundarytype == MF_BOUNDARY) {
			gdata->only_internal = true;

			doCommand(MF_CALC_BOUND_CONDITIONS, INTEGRATOR_STEP_1);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
			doCommand(MF_UPDATE_BOUND_VALUES, INTEGRATOR_STEP_1);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
		}

		// for SPS viscosity, compute first array of tau and exchange with neighbors
		if (problem->get_simparams()->visctype == SPSVISC) {
			gdata->only_internal = true;
			doCommand(SPS, INTEGRATOR_STEP_1);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_TAU);
		}

		// for k-eps viscosity, compute mean scalar strain and exchange with neighbors
		if (problem->get_simparams()->visctype == KEPSVISC) {
			gdata->only_internal = true;
			doCommand(MEAN_STRAIN, INTEGRATOR_STEP_1);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_STRAIN_RATE);
		}

		// compute forces only on internal particles
		gdata->only_internal = true;
		doCommand(FORCES, INTEGRATOR_STEP_1);
		// update forces of external particles
		if (MULTI_DEVICE)
			doCommand(UPDATE_EXTERNAL, BUFFER_FORCES);

		//MM		fetch/update forces on neighbors in other GPUs/nodes
		//				initially done trivial and slow: stop and read
		//			//reduce bodies

		// integrate also the externals
		gdata->only_internal = false;
		doCommand(EULER, INTEGRATOR_STEP_1);

		// this made sense for testing and running EULER on internals only
		//if (MULTI_DEVICE)
		//doCommand(UPDATE_EXTERNAL, BUFFER_POS | BUFFER_VEL | DBLBUFFER_WRITE);

		//			//reduce bodies
		//MM		fetch/update forces on neighbors in other GPUs/nodes
		//				initially done trivial and slow: stop and read

		// variable gravity
		if (problem->get_simparams()->gcallback) {
			// ask the Problem to update gravity, one per process
			doCallBacks();
			// upload on the GPU, one per device
			doCommand(UPLOAD_GRAVITY);
		}

		// semi-analytical boundary update
		if (problem->get_simparams()->boundarytype == MF_BOUNDARY) {
			gdata->only_internal = true;

			doCommand(MF_CALC_BOUND_CONDITIONS, INTEGRATOR_STEP_2);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
			doCommand(MF_UPDATE_BOUND_VALUES, INTEGRATOR_STEP_2);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
			doCommand(MF_UPDATE_GAMMA, INTEGRATOR_STEP_2, gdata->dt);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_GRADGAMMA);

			gdata->swapDeviceBuffers(BUFFER_GRADGAMMA);
		}

		// for SPS viscosity, compute first array of tau and exchange with neighbors
		if (problem->get_simparams()->visctype == SPSVISC) {
			gdata->only_internal = true;
			doCommand(SPS, INTEGRATOR_STEP_2);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_TAU);
		}

		// for k-eps viscosity, compute mean scalar strain and exchange with neighbors
		if (problem->get_simparams()->visctype == KEPSVISC) {
			gdata->only_internal = true;
			doCommand(MEAN_STRAIN, INTEGRATOR_STEP_2);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_STRAIN_RATE);
		}

		gdata->only_internal = true;
		doCommand(FORCES, INTEGRATOR_STEP_2);
		// update forces of external particles
		if (MULTI_DEVICE)
			doCommand(UPDATE_EXTERNAL, BUFFER_FORCES);

		// reduce bodies
		if (problem->get_simparams()->numODEbodies > 0) {
			doCommand(REDUCE_BODIES_FORCES);

			float3* totForce = new float3[problem->get_simparams()->numODEbodies];
			float3* totTorque = new float3[problem->get_simparams()->numODEbodies];

			// now sum up the partial forces and momenta computed in each gpu
			for (uint ob = 0; ob < problem->get_simparams()->numODEbodies; ob ++) {

				totForce[ob] = make_float3( 0.0F );
				totTorque[ob] = make_float3( 0.0F );

				for (int d = 0; d < gdata->devices; d++) {
					totForce[ob] += gdata->s_hRbTotalForce[d][ob];
					totTorque[ob] += gdata->s_hRbTotalTorque[d][ob];
				} // iterate on devices
			} // iterate on objects

			// if running multinode, also reduce across nodes
			if (MULTI_NODE) {
				// to minimize the overhead, we reduce the whole arrays of forces and torques in one command
				gdata->networkManager->networkFloatReduction((float*)totForce, 3 * problem->get_simparams()->numODEbodies, SUM_REDUCTION);
				gdata->networkManager->networkFloatReduction((float*)totTorque, 3 * problem->get_simparams()->numODEbodies, SUM_REDUCTION);
			}

			problem->ODE_bodies_timestep(totForce, totTorque, 2, gdata->dt, gdata->s_hRbGravityCenters, gdata->s_hRbTranslations, gdata->s_hRbRotationMatrices);

			// upload translation vectors and rotation matrices; will upload CGs after euler
			doCommand(UPLOAD_OBJECTS_MATRICES);
		} // if there are objects

		// integrate also the externals
		gdata->only_internal = false;
		doCommand(EULER, INTEGRATOR_STEP_2);

		if (problem->get_simparams()->boundarytype == MF_BOUNDARY) {
			gdata->only_internal = true;
			doCommand(MF_UPDATE_GAMMA, INTEGRATOR_STEP_2, gdata->dt);
			if (MULTI_DEVICE)
				doCommand(UPDATE_EXTERNAL, BUFFER_GRADGAMMA);
			gdata->swapDeviceBuffers(BUFFER_GRADGAMMA);
		}

		// euler needs the previous centers of gravity, so we upload CGs only here
		if (problem->get_simparams()->numODEbodies > 0)
			doCommand(UPLOAD_OBJECTS_CG);

		// this made sense for testing and running EULER on internals only
		//if (MULTI_DEVICE)
		//doCommand(UPDATE_EXTERNAL, BUFFER_POS | BUFFER_VEL);

		//			//reduce bodies

		gdata->swapDeviceBuffers(BUFFER_POS | BUFFER_VEL);

		if (problem->get_simparams()->visctype == KEPSVISC) {
			gdata->swapDeviceBuffers(BUFFER_TKE | BUFFER_EPSILON);
		}

		// increase counters
		gdata->iterations++;
		// to check, later, that the simulation is actually progressing
		float previous_t = gdata->t;
		gdata->t += gdata->dt;
		// buildneibs_freq?

		// choose minimum dt among the devices
		if (gdata->problem->get_simparams()->dtadapt) {
			gdata->dt = gdata->dts[0];
			for (int d = 1; d < gdata->devices; d++)
				gdata->dt = min(gdata->dt, gdata->dts[d]);
			// if runnign multinode, should also find the network minimum
			if (MULTI_NODE)
				gdata->networkManager->networkFloatReduction(&(gdata->dt), 1, MIN_REDUCTION);
		}

		// check that dt is not too small (absolute)
		if (!gdata->t) {
			throw DtZeroException(gdata->t, gdata->dt);
		} else if (gdata->dt < FLT_EPSILON) {
			fprintf(stderr, "FATAL: timestep %g under machine epsilon at iteration %lu - requesting quit...\n", gdata->dt, gdata->iterations);
			gdata->quit_request = true;
		}

		// check that dt is not too small (relative to t)
		if (gdata->t == previous_t) {
			fprintf(stderr, "FATAL: timestep %g too small at iteration %lu, time is still - requesting quit...\n", gdata->dt, gdata->iterations);
			gdata->quit_request = true;
		}

		//printf("Finished iteration %lu, time %g, dt %g\n", gdata->iterations, gdata->t, gdata->dt);

		bool finished = gdata->problem->finished(gdata->t);
		bool need_write = gdata->problem->need_write(gdata->t) || finished || gdata->quit_request;

		if (need_write) {
			// if we are about to quit, we want to save regardless --nosave option
			bool final_save = (finished || gdata->quit_request);

			if (final_save)
				printf("Issuing final save...\n");

			// set the buffers to be dumped
			uint which_buffers = BUFFER_POS | BUFFER_VEL | BUFFER_INFO;

			// choose the read buffer for the double buffered arrays
			which_buffers |= DBLBUFFER_READ;

			// compute and dump voriticity if set
			if (gdata->problem->get_simparams()->vorticity) {
				doCommand(VORTICITY);
				which_buffers |= BUFFER_VORTICITY;
			}

			// compute and dump normals if set
			// Warning: in the original code, buildneibs is called before surfaceParticle(). However, here should be safe
			// not to call, since it has been called at least once for sure
			if (gdata->problem->get_simparams()->surfaceparticle) {
				doCommand(SURFACE_PARTICLES);
				gdata->swapDeviceBuffers(BUFFER_INFO);
				which_buffers |= BUFFER_NORMALS;
			}

			if ( !gdata->nosave || final_save ) {
				// TODO: the performanceCounter could be "paused" here
				// dump what we want to save
				doCommand(DUMP, which_buffers);
				// triggers Writer->write()
				doWrite();
			} else
				// --nosave enabled, not final: just pretend we actually saved
				gdata->problem->mark_written(gdata->t);

			printStatus();
		}

		if (finished || gdata->quit_request)
			// NO doCommand() after keep_going has been unset!
			gdata->keep_going = false;
	}

	// NO doCommand() nor other barriers than the standard ones after the

	printf("Simulation end, cleaning up...\n");

	// doCommand(QUIT) would be equivalent, but this is more clear
	gdata->nextCommand = QUIT;
	gdata->threadSynchronizer->barrier(); // unlock CYCLE BARRIER 2
	gdata->threadSynchronizer->barrier(); // end of SIMULATION, begins FINALIZATION ***

	// just wait or...?

	gdata->threadSynchronizer->barrier(); // end of FINALIZATION ***

	// after the last barrier has been reached by all threads (or after the Synchronizer has been forcedly unlocked),
	// we wait for the threads to actually exit
	for (int d = 0; d < gdata->devices; d++)
		gdata->GPUWORKERS[d]->join_worker();

	return true;
}

// Allocate the shared buffers, i.e. those accessed by all workers
// Returns the number of allocated bytes.
// This does *not* include what was previously allocated (e.g. particles in problem->fillparts())
long unsigned int GPUSPH::allocateGlobalHostBuffers()
{

	long unsigned int numparts = gdata->totParticles;
	unsigned int numcells = gdata->nGridCells;
	const size_t floatSize = sizeof(float) * numparts;
	const size_t float3Size = sizeof(float3) * numparts;
	const size_t float4Size = sizeof(float4) * numparts;
	const size_t double4Size = sizeof(double4) * numparts;
	const size_t infoSize = sizeof(particleinfo) * numparts;
	const size_t hashSize = sizeof(hashKey) * numparts;
	const size_t vertexSize = sizeof(vertexinfo) * numparts;
	const size_t ucharCellSize = sizeof(uchar) * numcells;
	const size_t uintCellSize = sizeof(uint) * numcells;

	long unsigned int totCPUbytes = 0;

	// allocate pinned memory
	//s_hPos = (float*)
	// test also cudaHostAllocWriteCombined
	//cudaHostAlloc(&s_hPos, memSize4, cudaHostAllocPortable);
	//cudaMallocHost(&s_hPos, memSize4, cudaHostAllocPortable);
	gdata->s_hdPos = new double4[numparts];
	memset(gdata->s_hdPos, 0, double4Size);
	totCPUbytes += double4Size;

	gdata->s_hPos = new float4[numparts];
	memset(gdata->s_hPos, 0, float4Size);
	totCPUbytes += float4Size;

	gdata->s_hParticleHash = new hashKey[numparts];
	memset(gdata->s_hParticleHash, 0, hashSize);

	gdata->s_hVel = new float4[numparts];
	memset(gdata->s_hVel, 0, float4Size);
	totCPUbytes += float4Size;

	gdata->s_hInfo = new particleinfo[numparts];
	memset(gdata->s_hInfo, 0, infoSize);
	totCPUbytes += infoSize;

	if (problem->m_simparams.boundarytype == MF_BOUNDARY) {
		gdata->s_hVertices = new vertexinfo[numparts];
		memset(gdata->s_hVertices, 0, vertexSize);
		totCPUbytes += vertexSize;

		gdata->s_hBoundElement = new float4[numparts];
		memset(gdata->s_hBoundElement, 0, float4Size);
		totCPUbytes += float4Size;
	}

	if (problem->m_simparams.visctype == KEPSVISC) {
		gdata->s_hTKE = new float[numparts];
		memset(gdata->s_hTKE, 0, floatSize);
		totCPUbytes += floatSize;

		gdata->s_hEps = new float[numparts];
		memset(gdata->s_hEps, 0, floatSize);
		totCPUbytes += floatSize;
	}

	if (problem->m_simparams.vorticity) {
		gdata->s_hVorticity = new float3[numparts];
		memset(gdata->s_hVorticity, 0, float3Size);
		totCPUbytes += float3Size;
	}

	if (problem->m_simparams.surfaceparticle) {
		gdata->s_hNormals = new float4[numparts];
		memset(gdata->s_hNormals, 0, float4Size);
		totCPUbytes += float4Size;
	}

	if (gdata->numPlanes > 0) {
		if (gdata->numPlanes > MAXPLANES) {
			printf("FATAL: unsupported number of planes (%u > %u)\n", gdata->numPlanes, MAXPLANES);
			exit(1);
		}
		const size_t planeSize4 = sizeof(float4) * gdata->numPlanes;
		const size_t planeSize  = sizeof(float) * gdata->numPlanes;

		gdata->s_hPlanes = new float4[gdata->numPlanes];
		memset(gdata->s_hPlanes, 0, planeSize4);
		totCPUbytes += planeSize4;

		gdata->s_hPlanesDiv = new float[gdata->numPlanes];
		memset(gdata->s_hPlanes, 0, planeSize);
		totCPUbytes += planeSize;
	}

	/*dump_hPos = new float4[numparts];
	 memset(dump_hPos, 0, float4Size);
	 totCPUbytes += float4Size;

	 dump_hVel = new float4[numparts];
	 memset(dump_hVel, 0, float4Size);
	 totCPUbytes += float4Size;

	 dump_hInfo = new particleinfo[numparts];
	 memset(dump_hInfo, 0, infoSize);
	 totCPUbytes += infoSize;*/

	if (MULTI_DEVICE) {
		// deviceMap
		gdata->s_hDeviceMap = new uchar[numcells];
		memset(gdata->s_hDeviceMap, 0, ucharCellSize);
		totCPUbytes += ucharCellSize;

		// cellStarts, cellEnds, segmentStarts of all devices. Array of device pointers stored on host
		gdata->s_dCellStarts = (uint**)calloc(gdata->devices, sizeof(uint*));
		gdata->s_dCellEnds =  (uint**)calloc(gdata->devices, sizeof(uint*));
		gdata->s_dSegmentsStart = (uint**)calloc(gdata->devices, sizeof(uint*));

		// few bytes... but still count them
		totCPUbytes += gdata->devices * sizeof(uint*) * 3;

		for (uint d=0; d < gdata->devices; d++) {
			// NOTE: not checking errors, similarly to the other allocations.
			// Also cudaHostAllocWriteCombined flag was tested but led to ~0.1MIPPS performance loss, with and without streams.
			cudaHostAlloc( &(gdata->s_dCellStarts[d]), numcells * sizeof(uint), cudaHostAllocPortable );
			cudaHostAlloc( &(gdata->s_dCellEnds[d]), numcells * sizeof(uint), cudaHostAllocPortable );
			// same on non-pinned memory
			//gdata->s_dCellStarts[d] = (uint*)calloc(numcells, sizeof(uint));
			//gdata->s_dCellEnds[d] =   (uint*)calloc(numcells, sizeof(uint));
			totCPUbytes += uintCellSize * 2;

			gdata->s_dSegmentsStart[d] = (uint*)calloc(4, sizeof(uint));
			totCPUbytes += sizeof(uint) * 4;
		}
	}
	return totCPUbytes;
}

// Deallocate the shared buffers, i.e. those accessed by all workers
void GPUSPH::deallocateGlobalHostBuffers() {
	// planes
	if (gdata->numPlanes > 0) {
		delete[] gdata->s_hPlanes;
		delete[] gdata->s_hPlanesDiv;
	}
	//cudaFreeHost(s_hPos); // pinned memory
	delete[] gdata->s_hdPos;
	delete[] gdata->s_hPos;
	delete[] gdata->s_hParticleHash;
	delete[] gdata->s_hVel;
	delete[] gdata->s_hInfo;
	if (gdata->s_hVertices)
		delete[] gdata->s_hVertices;
	if (gdata->s_hBoundElement)
		delete[] gdata->s_hBoundElement;
	if (gdata->s_hTKE)
		delete[] gdata->s_hTKE;
	if (gdata->s_hEps)
		delete[] gdata->s_hEps;

	if (problem->m_simparams.vorticity)
		delete[] gdata->s_hVorticity;
	if (problem->m_simparams.surfaceparticle)
		delete[] gdata->s_hNormals;
	// multi-GPU specific arrays
	if (MULTI_DEVICE) {
		delete[] gdata->s_hDeviceMap;
		// cells
		for (int d = 0; d < gdata->devices; d++) {
			cudaFreeHost(gdata->s_dCellStarts[d]);
			cudaFreeHost(gdata->s_dCellEnds[d]);
			//delete[] gdata->s_dCellStarts[d];
			//delete[] gdata->s_dCellEnds[d];
			// same on non-pinned memory
			delete[] gdata->s_dSegmentsStart[d];
		}
		delete[] gdata->s_dCellEnds;
		delete[] gdata->s_dCellStarts;
		delete[] gdata->s_dSegmentsStart;
	}
}

// Sort the particles in-place (pos, vel, info) according to the device number;
// update counters s_hPartsPerDevice and s_hStartPerDevice, which will be used to upload
// and download the buffers. Finally, initialize s_dSegmentsStart
// Assumptions: problem already filled, deviceMap filled, particles copied in shared arrays
void GPUSPH::sortParticlesByHash() {
	// DEBUG: print the list of particles before sorting
	// for (uint p=0; p < gdata->totParticles; p++)
	//	printf(" p %d has id %u, dev %d\n", p, id(gdata->s_hInfo[p]), gdata->calcDevice(gdata->s_hPos[p]) );

	// count parts for each device, even in other nodes (s_hPartsPerDevice only includes devices in self node on)
	uint particlesPerGlobalDevice[MAX_DEVICES_PER_CLUSTER];

	// reset counters. Not using memset since sizes are smaller than 1Kb
	for (uint d = 0; d < MAX_DEVICES_PER_NODE; d++)    gdata->s_hPartsPerDevice[d] = 0;
	for (uint n = 0; n < MAX_NODES_PER_CLUSTER; n++)   gdata->processParticles[n]  = 0;
	for (uint d = 0; d < MAX_DEVICES_PER_CLUSTER; d++) particlesPerGlobalDevice[d] = 0;

	// TODO: move this in allocateGlobalBuffers...() and rename it, or use only here as a temporary buffer?
	uchar* m_hParticleHashes = new uchar[gdata->totParticles];

	// fill array with particle hashes (aka global device numbers) and increase counters
	for (uint p = 0; p < gdata->totParticles; p++) {

		// compute cell according to the particle's position and to the deviceMap
		uchar whichGlobalDev = gdata->calcGlobalDeviceIndex(gdata->s_hPos[p]);

		// that's the key!
		m_hParticleHashes[p] = whichGlobalDev;

		// increase node and globalDev counter (only useful for multinode)
		gdata->processParticles[gdata->RANK(whichGlobalDev)]++;

		particlesPerGlobalDevice[gdata->GLOBAL_DEVICE_NUM(whichGlobalDev)]++;

		// if particle is in current node, increment the device counters
		if (gdata->RANK(whichGlobalDev) == gdata->mpi_rank)
			// increment per-device counter
			gdata->s_hPartsPerDevice[gdata->DEVICE(whichGlobalDev)]++;

		//if (whichGlobalDev != 0)
		//printf(" ö part %u has key %u (n%dd%u) global dev %u \n", p, whichGlobalDev, gdata->RANK(whichGlobalDev), gdata->DEVICE(whichGlobalDev), gdata->GLOBAL_DEVICE_NUM(whichGlobalDev) );

	}

	// printParticleDistribution();

	// update s_hStartPerDevice with incremental sum (should do in specific function?)
	gdata->s_hStartPerDevice[0] = 0;
	// zero is true for the first node. For the next ones, need to sum the number of particles of the previous nodes
	if (MULTI_NODE)
		for (uint prev_nodes = 0; prev_nodes < gdata->mpi_rank; prev_nodes++)
			gdata->s_hStartPerDevice[0] += gdata->processParticles[prev_nodes];
	for (uint d = 1; d < gdata->devices; d++)
		gdata->s_hStartPerDevice[d] = gdata->s_hStartPerDevice[d-1] + gdata->s_hPartsPerDevice[d-1];

	// *** About the algorithm being used ***
	//
	// Since many particles share the same key, what we need is actually a compaction rather than a sort.
	// A cycle sort would be probably the best performing in terms of reducing the number of writes.
	// A selection sort would be the easiest to implement but would yield more swaps than needed.
	// The following variant, hybrid with a counting sort, is implemented.
	// We already counted how many particles are there for each device (m_partsPerDevice[]).
	// We keep two pointers, leftB and rightB (B stands for boundary). The idea is that leftB is the place
	// where we are going to put the next element and rightB is being moved to "scan" the rest of the array
	// and select next element. Unlike selection sort, rightB is initialized at the end of the array and
	// being decreased; this way, each element is expected to be moved no more than twice (estimation).
	// Moreover, a burst of particles which partially overlaps the correct bucket is not entirely moved:
	// since rightB goes from right to left, the rightmost particles are moved while the overlapping ones
	// are not. All particles before leftB have already been compacted; leftB is incremented as long as there
	// are particles already in correct positions. When there is a bucket change (we track it with nextBucketBeginsAt)
	// rightB is reset to the end of the array.
	// Possible optimization: decrease maxIdx to the last non-correct element of the array (if there is a burst
	// of correct particles in the end, this avoids scanning them everytime) and update this while running.
	// The following implementation iterates on buckets explicitly instead of working solely on leftB and rightB
	// and detect the bucket change. Same operations, cleaner code.

	// init
	const uint maxIdx = (gdata->totParticles - 1);
	uint leftB = 0;
	uint rightB;
	uint nextBucketBeginsAt = 0;

	// NOTE: in the for cycle we want to iterate on the global number of devices, not the local (process) one
	// NOTE(2): we don't need to iterate in the last bucket: it should be already correct after the others. That's why "devices-1".
	// We might want to iterate on last bucket only for correctness check.
	// For each bucket (device)...
	for (uint currentGlobalDevice = 0; currentGlobalDevice < (gdata->totDevices - 1); currentGlobalDevice++) {
		// compute where current bucket ends
		nextBucketBeginsAt += particlesPerGlobalDevice[currentGlobalDevice];
		// reset rightB to the end
		rightB = maxIdx;
		// go on until we reach the end of the current bucket
		while (leftB < nextBucketBeginsAt) {

			// translate from globalDeviceIndex to an absolute device index in 0..totDevices (the opposite convertDeviceMap does)
			uint currPartGlobalDevice = gdata->GLOBAL_DEVICE_NUM( m_hParticleHashes[leftB] );

			// if in the current position there is a particle *not* belonging to the bucket...
			if (currPartGlobalDevice != currentGlobalDevice) {

				// ...let's find a correct one, scanning from right to left
				while ( gdata->GLOBAL_DEVICE_NUM( m_hParticleHashes[rightB] ) != currentGlobalDevice) rightB--;

				// here it should never happen that (rightB <= leftB). We should throw an error if it happens
				particleSwap(leftB, rightB);
				std::swap(m_hParticleHashes[leftB], m_hParticleHashes[rightB]);
			}

			// already correct or swapped, time to go on
			leftB++;
		}
	}
	// delete array of keys (might be recycled instead?)
	delete[] m_hParticleHashes;

	// initialize the outer cells values in s_dSegmentsStart. The inner_edge are still uninitialized
	for (uint currentDevice = 0; currentDevice < gdata->devices; currentDevice++) {
		uint assigned_parts = gdata->s_hPartsPerDevice[currentDevice];
		printf("    d%u  p %u\n", currentDevice, assigned_parts);
		// this should always hold according to the current CELL_TYPE values
		gdata->s_dSegmentsStart[currentDevice][CELLTYPE_INNER_CELL ] = 		EMPTY_SEGMENT;
		// this is usually not true, since a device usually has neighboring cells; will be updated at first reorder
		gdata->s_dSegmentsStart[currentDevice][CELLTYPE_INNER_EDGE_CELL ] =	EMPTY_SEGMENT;
		// this is true and will change at first APPEND
		gdata->s_dSegmentsStart[currentDevice][CELLTYPE_OUTER_EDGE_CELL ] =	EMPTY_SEGMENT;
		// this is true and might change between a reorder and the following crop
		gdata->s_dSegmentsStart[currentDevice][CELLTYPE_OUTER_CELL ] =		EMPTY_SEGMENT;
	}

	// DEBUG: check if the sort was correct
	/* bool monotonic = true;
	bool count_c = true;
	uint hcount[MAX_DEVICES_PER_NODE];
	for (uint d=0; d < MAX_DEVICES_PER_NODE; d++)
		hcount[d] = 0;
	for (uint p=0; p < gdata->totParticles && monotonic; p++) {
		uint cdev = gdata->calcGlobalDeviceIndex( gdata->s_hPos[p] );
		uint pdev;
		if (p > 0) pdev = gdata->calcGlobalDeviceIndex( gdata->s_hPos[p-1] );
		if (p > 0 && cdev < pdev ) {
			printf(" -- sorting error: array[%d] has device n%dd%u, array[%d] has device n%dd%u (skipping next errors)\n",
				p-1, gdata->RANK(pdev), gdata->	DEVICE(pdev), p, gdata->RANK(cdev), gdata->	DEVICE(cdev) );
			monotonic = false;
		}
		// count particles of the current process
		if (gdata->RANK(cdev) == gdata->mpi_rank)
			hcount[ gdata->DEVICE(cdev) ]++;
	}
	// WARNING: the following check is only for particles of the current rank (multigpu, not multinode).
	// Each process checks its own particles.
	for (uint d=0; d < gdata->devices; d++)
		if (hcount[d] != gdata->s_hPartsPerDevice[d]) {
			count_c = false;
			printf(" -- sorting error: counted %d particles for device %d, but should be %d\n",
				hcount[d], d, gdata->s_hPartsPerDevice[d]);
		}
	if (monotonic && count_c)
		printf(" --- array OK\n");
	else
		printf(" --- array ERROR\n");
	// finally, print the list again
	//for (uint p=1; p < gdata->totParticles && monotonic; p++)
		//printf(" p %d has id %u, dev %d\n", p, id(gdata->s_hInfo[p]), gdata->calcDevice(gdata->s_hPos[p]) ); // */
}

// Swap two particles in shared arrays (pos, vel, pInfo); used in host sort
void GPUSPH::particleSwap(uint idx1, uint idx2)
{
	// could keep a counter
	swap(gdata->s_hPos[idx1], gdata->s_hPos[idx2]);
	swap(gdata->s_hVel[idx1], gdata->s_hVel[idx2]);
	swap(gdata->s_hInfo[idx1], gdata->s_hInfo[idx2]);
}

// set nextCommand, unlock the threads and wait for them to complete
void GPUSPH::doCommand(CommandType cmd, uint flags, float arg)
{
	// resetting the host buffers is useful to check if the arrays are completely filled
	/*/ if (cmd==DUMP) {
	 const uint float4Size = sizeof(float4) * gdata->totParticles;
	 const uint infoSize = sizeof(particleinfo) * gdata->totParticles;
	 memset(gdata->s_hPos, 0, float4Size);
	 memset(gdata->s_hVel, 0, float4Size);
	 memset(gdata->s_hInfo, 0, infoSize);
	 } */
	gdata->nextCommand = cmd;
	gdata->commandFlags = flags;
	gdata->extraCommandArg = arg;
	gdata->threadSynchronizer->barrier(); // unlock CYCLE BARRIER 2
	gdata->threadSynchronizer->barrier(); // wait for completion of last command and unlock CYCLE BARRIER 1
}

void GPUSPH::setViscosityCoefficient()
{
	PhysParams *pp = gdata->problem->get_physparams();
	// Setting visccoeff
	switch (gdata->problem->get_simparams()->visctype) {
		case ARTVISC:
			pp->visccoeff = pp->artvisccoeff;
			break;

		case KINEMATICVISC:
		case SPSVISC:
			pp->visccoeff = 4.0*pp->kinematicvisc;
			break;

		case DYNAMICVISC:
			pp->visccoeff = pp->kinematicvisc;
			break;
	}
}

// creates the Writer according to the requested WriterType
void GPUSPH::createWriter()
{
	//gdata->writerType = gdata->problem->get_writertype();
	//switch(gdata->writerType) {
	switch (gdata->problem->get_writertype()) {
		case Problem::TEXTWRITER:
			gdata->writerType = TEXTWRITER;
			gdata->writer = new TextWriter(gdata->problem);
			break;

		case Problem::VTKWRITER:
			gdata->writerType = VTKWRITER;
			gdata->writer = new VTKWriter(gdata->problem);
			gdata->writer->setGlobalData(gdata); // VTK also supports writing the device index
			break;

		case Problem::VTKLEGACYWRITER:
			gdata->writerType = VTKLEGACYWRITER;
			gdata->writer = new VTKLegacyWriter(gdata->problem);
			break;

		case Problem::CUSTOMTEXTWRITER:
			gdata->writerType = CUSTOMTEXTWRITER;
			gdata->writer = new CustomTextWriter(gdata->problem);
			break;

		case Problem::UDPWRITER:
			gdata->writerType = UDPWRITER;
			gdata->writer = new UDPWriter(gdata->problem);
			break;

		default:
			//stringstream ss;
			//ss << "Writer not supported";
			//throw runtime_error(ss.str());
			printf("Writer not supported\n");
			break;
	}
}

void GPUSPH::doWrite()
{
	uint node_offset = gdata->s_hStartPerDevice[0];

	// TODO MERGE REVIEW. do on each node separately?
	double3 const& wo = problem->get_worldorigin();
	for (uint i = 0; i < gdata->totParticles; i++) {
		const float4 pos = gdata->s_hPos[i];
		double4 dpos;
		uint3 gridPos = gdata->calcGridPosFromHash(gdata->s_hParticleHash[i]);
		dpos.x = ((double) gdata->cellSize.x)*(gridPos.x + 0.5) + (double) pos.x + wo.x;
		dpos.y = ((double) gdata->cellSize.y)*(gridPos.y + 0.5) + (double) pos.y + wo.y;
		dpos.z = ((double) gdata->cellSize.z)*(gridPos.z + 0.5) + (double) pos.z + wo.z;

		gdata->s_hdPos[i] = dpos;
	}
	gdata->writer->write(gdata->processParticles[gdata->mpi_rank],
		gdata->s_hdPos + node_offset, gdata->s_hVel + node_offset, gdata->s_hInfo + node_offset,
		( gdata->s_hVorticity ? gdata->s_hVorticity + node_offset : NULL),
		gdata->t, gdata->problem->get_simparams()->testpoints,
		( gdata->s_hNormals ? gdata->s_hNormals + node_offset : NULL));
	gdata->problem->mark_written(gdata->t);

	// TODO: enable energy computation and dump
	/*calc_energy(m_hEnergy,
		m_dPos[m_currentPosRead],
		m_dVel[m_currentVelRead],
		m_dInfo[m_currentInfoRead],
		m_numParticles,
		m_physparams->numFluids);
	m_writer->write_energy(m_simTime, m_hEnergy);*/
}

void GPUSPH::buildNeibList()
{
	// run most of the following commands on all particles
	gdata->only_internal = false;

	doCommand(CALCHASH);
	doCommand(SORT);
	doCommand(REORDER);

	// swap pos, vel and info double buffers
	gdata->swapDeviceBuffers(BUFFERS_ALL_DBL);

	// if running on multiple GPUs, update the external cells
	if (MULTI_DEVICE) {
		// copy cellStarts, cellEnds and segments on host
		doCommand(DUMP_CELLS);
		doCommand(UPDATE_SEGMENTS);

		// here or later, before update indices: MPI_Allgather (&sendbuf,sendcount,sendtype,&recvbuf, recvcount,recvtype,comm)
		// maybe overlapping with dumping cells (run async before dumping the cells)

		// update particle offsets
		updateArrayIndices();
		// crop external cells
		doCommand(CROP);
		// append fresh copies of the externals
		doCommand(APPEND_EXTERNAL, BUFFER_POS | BUFFER_VEL | BUFFER_INFO | DBLBUFFER_READ);
	}

	// build neib lists only for internal particles
	gdata->only_internal = true;
	doCommand(BUILDNEIBS);
}

void GPUSPH::doCallBacks()
{
	Problem *pb = gdata->problem;

	if (pb->m_simparams.mbcallback)
		gdata->s_mbData = pb->get_mbdata(
			gdata->t,
			gdata->dt,
			gdata->iterations == 0);

	if (pb->m_simparams.gcallback)
		gdata->s_varGravity = pb->g_callback(gdata->t);
}

void GPUSPH::initializeGammaAndGradGamma()
{
	buildNeibList();

	gdata->only_internal = true;

	doCommand(MF_INIT_GAMMA);
	if (MULTI_DEVICE)
		doCommand(UPDATE_EXTERNAL, BUFFER_POS | BUFFER_VEL | BUFFER_GRADGAMMA | DBLBUFFER_WRITE);
	gdata->swapDeviceBuffers(BUFFER_POS | BUFFER_VEL | BUFFER_GRADGAMMA);

	buildNeibList();

	// Compute virtual displacement
	uint itNumber = 200;
	float deltat = 1.0/itNumber;

	for (uint i = 0; i < itNumber; ++i) {
		// On every iteration updateGamma() is called twice, while updatePositions() is called only once,
		// since evolution equation for gamma is integrated in time with a second-order time scheme:
		// gamma(n+1) = gamma(n) + 0.5*[gradGam(n) + gradGam(n+1)]*[r(n+1) - r(n)]

		// Update gamma 1st call
		doCommand(MF_UPDATE_GAMMA, INITIALIZATION_STEP, deltat);
		if (MULTI_DEVICE)
			doCommand(UPDATE_EXTERNAL, BUFFER_GRADGAMMA | DBLBUFFER_WRITE);
		gdata->swapDeviceBuffers(BUFFER_GRADGAMMA);

		// Move the particles
		doCommand(MF_UPDATE_POS, deltat);
		if (MULTI_DEVICE)
			doCommand(UPDATE_EXTERNAL, BUFFER_POS | DBLBUFFER_WRITE);
		gdata->swapDeviceBuffers(BUFFER_POS);

		buildNeibList();

		doCommand(MF_UPDATE_GAMMA, INITIALIZATION_STEP, deltat);
		if (MULTI_DEVICE)
			doCommand(UPDATE_EXTERNAL, BUFFER_GRADGAMMA | DBLBUFFER_WRITE);
		gdata->swapDeviceBuffers(BUFFER_GRADGAMMA);
	}
}

void GPUSPH::imposeDynamicBoundaryConditions()
{
	gdata->only_internal = true;
	doCommand(MF_CALC_BOUND_CONDITIONS, INITIALIZATION_STEP);
	if (MULTI_DEVICE)
		doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
}

void GPUSPH::updateValuesAtBoundaryElements()
{
	gdata->only_internal = true;
	doCommand(MF_UPDATE_BOUND_VALUES, INITIALIZATION_STEP);
	if (MULTI_DEVICE)
		doCommand(UPDATE_EXTERNAL, BUFFER_VEL | BUFFER_TKE | BUFFER_EPSILON | BUFFER_PRESSURE);
}

void GPUSPH::printStatus()
{
//#define ti timingInfo
	printf(	"Simulation time t=%es, iteration=%s, dt=%es, %s parts (%.2g MIPPS), %u files saved so far\n",
			//"mean %e neibs. in %es, %e neibs/s, max %u neibs\n"
			//"mean neib list in %es\n"
			//"mean integration in %es\n",
			gdata->t, gdata->addSeparators(gdata->iterations).c_str(), gdata->dt,
			gdata->addSeparators(gdata->totParticles).c_str(), m_performanceCounter->getMIPPS(gdata->iterations * gdata->totParticles),
			gdata->writer->getLastFilenum()
			//ti.t, ti.iterations, ti.dt, ti.numParticles, (double) ti.meanNumInteractions,
			//ti.meanTimeInteract, ((double)ti.meanNumInteractions)/ti.meanTimeInteract, ti.maxNeibs,
			//ti.meanTimeNeibsList,
			//ti.meanTimeEuler
			);
	fflush(stdout);
//#undef ti
}

void GPUSPH::printParticleDistribution()
{
	printf("Particle distribution for process %u at iteration %lu:\n", gdata->mpi_rank, gdata->iterations);
	for (uint d = 0; d < gdata->devices; d++)
		printf(" - Device %u: %u particles\n", d, gdata->s_hPartsPerDevice[d]);
	printf("   TOT:   %u particles\n", gdata->processParticles[ gdata->mpi_rank ]);
}
// Do a roll call of particle IDs; useful after dumps if the filling was uniform.
// Notifies anomalies only once in the simulation for each particle ID
// NOTE: only meaningful in singlenode (otherwise, there is no correspondence between indices and ids)
void GPUSPH::rollCallParticles()
{
	bool all_normal = true;

	// reset bitmap and addrs
	for (uint idx = 0; idx < gdata->processParticles[gdata->mpi_rank]; idx++) {
		m_rcBitmap[idx] = false;
		m_rcAddrs[idx] = 0xFFFFFFFF;
	}

	// fill out the bitmap and check for duplicates
	for (uint pos = 0; pos < gdata->processParticles[gdata->mpi_rank]; pos++) {
		uint idx = id(gdata->s_hInfo[pos]);
		if (m_rcBitmap[idx] && !m_rcNotified[idx]) {
			printf("WARNING: at iteration %lu, time %g particle idx %u is in pos %u and %u!\n",
					gdata->iterations, gdata->t, idx, m_rcAddrs[idx], pos);
			// getchar(); // useful for debugging
			// printf("Press ENTER to continue...\n");
			all_normal = false;
			m_rcNotified[idx] = true;
		}
		m_rcBitmap[idx] = true;
		m_rcAddrs[idx] = pos;
	}
	// now check if someone is missing
	for (uint idx = 0; idx < gdata->processParticles[gdata->mpi_rank]; idx++)
		if (!m_rcBitmap[idx] && !m_rcNotified[idx]) {
			printf("WARNING: at iteration %lu, time %g particle idx %u was not found!\n",
					gdata->iterations, gdata->t, idx);
			// printf("Press ENTER to continue...\n");
			// getchar(); // useful for debugging
			m_rcNotified[idx] = true;
			all_normal = false;
		}
	// if there was any warning...
	if (!all_normal) {
		printf("Recap of devices after roll call:\n");
		for (uint d = 0; d < gdata->devices; d++) {
			printf(" - device at index %u has %s particles assigned and offset %s\n", d,
					gdata->addSeparators(gdata->s_hPartsPerDevice[d]).c_str(),
					gdata->addSeparators(gdata->s_hStartPerDevice[d]).c_str() );
			// extra stuff for deeper debugging
			// uint last_idx = gdata->s_hStartPerDevice[d] + gdata->s_hPartsPerDevice[d] - 1;
			// uint first_idx = gdata->s_hStartPerDevice[d];
			// printf("   first part has idx %u, last part has idx %u\n", id(gdata->s_hInfo[first_idx]), id(gdata->s_hInfo[last_idx])); */
		}
	}
}

// update s_hStartPerDevice, s_hPartsPerDevice and totParticles
// Could go in GlobalData but would need another forward-declaration
void GPUSPH::updateArrayIndices() {
	uint processCount = 0;

	// just store an incremental counter
	for (uint d = 0; d < gdata->devices; d++)
		processCount += gdata->s_hPartsPerDevice[d] = gdata->GPUWORKERS[d]->getNumInternalParticles();

	// update che number of particles of the current process. Do we need to store the previous value?
	// uint previous_process_parts = gdata->processParticles[ gdata->mpi_rank ];
	gdata->processParticles[gdata->mpi_rank] = processCount;

	// allgather values, aka: receive values of other processes
	if (MULTI_NODE)
		gdata->networkManager->allGatherUints(&processCount, gdata->processParticles);

	// now update the offsets for each device:
	gdata->s_hStartPerDevice[0] = 0;
	for (uint n = 0; n < gdata->mpi_rank; n++) // first shift s_hStartPerDevice[0] by means of the previous nodes...
		gdata->s_hStartPerDevice[0] += gdata->processParticles[n];
	for (int d = 1; d < gdata->devices; d++) // ...then shift the other devices by means of the previous devices
		gdata->s_hStartPerDevice[d] = gdata->s_hStartPerDevice[d-1] + gdata->s_hPartsPerDevice[d-1];

	// process 0 checks if total number of particles varied in the simulation
	if (gdata->mpi_rank == 0 || true) {
		uint newSimulationTotal = 0;
		for (uint n = 0; n < gdata->mpi_nodes; n++)
			newSimulationTotal += gdata->processParticles[n];

		if (newSimulationTotal != gdata->totParticles) {
			printf("WARNING: at iteration %lu the number of particles changed from %u to %u for no known reason!\n",
				gdata->iterations, gdata->totParticles, newSimulationTotal);

			// who is missing? if single-node, do a roll call
			if (SINGLE_NODE) {
				doCommand(DUMP, BUFFER_INFO | DBLBUFFER_READ);
				rollCallParticles();
			}

			// update totParticles to avoid dumping an outdated particle (and repeating the warning).
			// Note: updading *after* the roll call likely shows the missing particle(s) and the duplicate(s). Doing before it only shows the missing one(s)
			gdata->totParticles = newSimulationTotal;
		}
	}
}

// initialize the centers of gravity of objects
void GPUSPH::initializeObjectsCGs()
{
	if (gdata->problem->get_simparams()->numODEbodies > 0) {
		gdata->s_hRbGravityCenters = gdata->problem->get_ODE_bodies_cg();

		// Debug
		/* for (int i=0; i < m_simparams->numbodies; i++) {
			printf("Body %d: cg(%g,%g,%g) lastindex: %d\n", i, cg[i].x, cg[i].y, cg[i].z, m_hRbLastIndex[i]);
		}
		uint rbfirstindex[MAXBODIES];
		CUDA_SAFE_CALL(cudaMemcpyFromSymbol(rbfirstindex, "d_rbstartindex", m_simparams->numbodies*sizeof(uint)));
		for (int i=0; i < m_simparams->numbodies; i++) {
			printf("Body %d: firstindex: %d\n", i, rbfirstindex[i]);
		} */
	}
}
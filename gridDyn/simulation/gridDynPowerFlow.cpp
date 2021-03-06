/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil;  eval: (c-set-offset 'innamespace 0); -*- */
/*
   * LLNS Copyright Start
 * Copyright (c) 2016, Lawrence Livermore National Security
 * This work was performed under the auspices of the U.S. Department
 * of Energy by Lawrence Livermore National Laboratory in part under
 * Contract W-7405-Eng-48 and in part under Contract DE-AC52-07NA27344.
 * Produced at the Lawrence Livermore National Laboratory.
 * All rights reserved.
 * For details, see the LICENSE file.
 * LLNS Copyright End
*/

#include "gridDyn.h"
#include "gridEvent.h"
#include "gridRecorder.h"
#include "vectorOps.hpp"
#include "eventQueue.h"
#include "gridBus.h"
#include "solvers/solverInterface.h"
#include "simulation/diagnostics.h"
#include "powerFlowErrorRecovery.h"
#include "gridDynSimulationFileOps.h"

#include "continuation.h"
//system headers
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iostream>


// --------------- power flow program ---------------

// power flow solver
int gridDynSimulation::powerflow ()
{

  const solverMode &sm = *defPowerFlowMode;
  int out = FUNCTION_EXECUTION_SUCCESS;
  count_t voltage_iteration_count = 0;
  count_t power_iteration_count = 0;
  double prevPower = 0;
  int retval = makeReady (gridState_t::INITIALIZED, sm);
  // load the vectors
  //operate in a loop then check then repeat
  bool powerAdjust = controlFlags[power_adjust_enabled];

  std::vector<double> slkBusBase (slkBusses.size ());

  auto pFlowData = getSolverInterface (sm);
  //Create the error recovery object to use if necessary
  powerFlowErrorRecovery pfer (this, pFlowData);

  if (pFlowData->size () > 0)        //handle the condition when all buses are swing buses hence nothing to solve
    {
      power_iteration_count = 0;
      do           //outer power distribution loop
        {
          if (powerAdjust)
            {
              prevPower = 0;
              for (size_t kk = 0; kk < slkBusses.size (); ++kk)
                {
                  slkBusBase[kk] = slkBusses[kk]->getGenerationReal ();
                  prevPower += slkBusBase[kk];
                }
            }
          voltage_iteration_count = 0;
          change_code AdjustmentChanges = change_code::no_change;
          do
            {
              guess (timeCurr, pFlowData->state_data (), nullptr,sm);

              // solve
              retval = pFlowData->solve (timeCurr, timeCurr);

              if (retval < 0)
                {
				  LOG_WARNING("solver error return");
			
                  if (controlFlags[no_powerflow_error_recovery])
                    {
                      LOG_ERROR ("unable to solve power flow ||" + pFlowData->getLastErrorString ());
					  
                      return retval;
                    }
                  auto prc = pfer.attemptFix (retval);
                  if (prc == powerFlowErrorRecovery::recovery_return_codes::out_of_options)
                    {
                      LOG_ERROR ("unable to solve power flow ||" + pFlowData->getLastErrorString ());
                      return retval;
                    }

                  continue;

                }
              if (pfer.attempts () > 0)
                {
                  if (!std::all_of (pFlowData->state_data (), pFlowData->state_data () + pFlowData->size (), [](double a) {
            return std::isfinite (a);
          }))
                    {
                      LOG_WARNING ("solver returned an infinite or nan");
                      retval = -30;
                    }
                }


              // pass the solution to the bus objects
              setState (timeCurr,pFlowData->state_data (),nullptr, sm);
              //tell the components to calculate some parameters and power flows
              updateLocalCache ();

              voltage_iteration_count++;

              if (voltage_iteration_count > max_Vadjust_iterations)
                {
                  LOG_WARNING ("WARNING::Voltage Loop iteration count limit exceeded");
                  break;
                }

              if (!controlFlags[no_powerflow_adjustments])
                {
                  //check the solution if voltage limits are not ignored

                  if (pState == gridState_t::INITIALIZED)
                    {
                      if (controlFlags[first_run_limits_only])
                        {
                          AdjustmentChanges = powerFlowAdjust (0, check_level_t::reversable_only);
                        }
                      else
                        {
                          AdjustmentChanges = powerFlowAdjust (lower_flags (controlFlags), check_level_t::reversable_only);
                        }

                    }
                  else
                    {
                      AdjustmentChanges = powerFlowAdjust (lower_flags (controlFlags), check_level_t::reversable_only);
                    }

                  if (AdjustmentChanges > change_code::non_state_change)
                    {
                      reInitpFlow (sm, AdjustmentChanges);
                    }
                  if (AdjustmentChanges == change_code::no_change)                             //if there was no adjustable changes  check if there was any non-reversable checks
                    {
                      AdjustmentChanges = powerFlowAdjust (lower_flags (controlFlags), check_level_t::full_check);
                      if (AdjustmentChanges > change_code::no_change)
                        {
                          checkNetwork (network_check_type::simplified);
                          if (AdjustmentChanges == change_code::state_count_change)
                            {
                              reInitpFlow (sm, AdjustmentChanges);
                            }
                        }

                    }
                }
              else
                {
                  AdjustmentChanges = change_code::no_change;
                }
            }
          while ((retval<0)||(AdjustmentChanges != change_code::no_change));

          if (controlFlags[power_adjust_enabled])
            {

              powerAdjust = loadBalance (prevPower,slkBusBase);
              if (powerAdjust)
                {
                  if (!controlFlags[no_reset])
                    {
                      reset (reset_levels::minimal);
                    }
                  if (opFlags[state_change_flag])
                    {
                      reInitpFlow (sm);
                    }
                }
              power_iteration_count++;
              if (power_iteration_count > max_Padjust_iterations)
                {
                  LOG_WARNING ("WARNING::Power Adjust Loop iteration count limit exceeded" );
                  break;
                }


            }
        }
      while (powerAdjust);
      // solver stats
// SGS Kinsol Log file info here sometime Woodard
      if ((consolePrintLevel >= GD_TRACE_PRINT)||(logPrintLevel >= GD_TRACE_PRINT))
        {
          pFlowData->logSolverStats (GD_TRACE_PRINT);
        }

    }
  else
    {
      setState (timeCurr, nullptr,nullptr, sm);
      updateLocalCache ();
    }

  if (pState == gridState_t::INITIALIZED)
    {
      if ((controlFlags[save_power_flow_data])&& (!opFlags[powerflow_saved]))
        {
          savePowerFlow (this,powerFlowFile);
          opFlags[powerflow_saved] = true;
        }
    }
  //store the results to the buses
  pState = gridState_t::POWERFLOW_COMPLETE;

  return out;
}



int gridDynSimulation::reInitpFlow (const solverMode &sMode,change_code change)
{


  if (opFlags[slack_bus_change])
    {
      checkNetwork (network_check_type::full);
    }
  else if (opFlags[connectivity_change_flag])
    {
      checkNetwork (network_check_type::simplified);
    }
  if (opFlags[reset_voltage_flag])
    {
      reset (reset_levels::full);
      opFlags.reset (reset_voltage_flag);
    }

  auto pFlowData = getSolverInterface (sMode);
  int retval = FUNCTION_EXECUTION_SUCCESS;
  if ((opFlags[state_change_flag]) || (change == change_code::state_count_change))
    {
      updateOffsets (sMode);
      auto ssize = stateSize (sMode);
      retval = pFlowData->allocate (ssize);
      retval = pFlowData->initialize (timeCurr);
      pState = gridState_t::INITIALIZED;
    }
  else if ((opFlags[object_change_flag]) || (change == change_code::object_change))
    {

      if (pState > gridState_t::POWERFLOW_COMPLETE)
        {      //we have to reset for the dynamic computation
          auto ssize = stateSize (sMode);
          if (ssize != pFlowData->size ())
            {
              updateOffsets (sMode);
              retval = pFlowData->allocate (ssize);
              retval = pFlowData->initialize (timeCurr);
              pState = gridState_t::INITIALIZED;
            }
        }
      pFlowData->setMaxNonZeros (jacSize (sMode));
      if (!controlFlags[dense_solver])
        {
          if ((retval = pFlowData->sparseReInit (solverInterface::sparse_reinit_modes::resize)) != FUNCTION_EXECUTION_SUCCESS)
            {
              return retval;
            }
        }
    }
  else
    {
      if (pState > gridState_t::DYNAMIC_INITIALIZED)
        {        //we have to reset for the dynamic computation
          auto ssize = stateSize (sMode);
          if (ssize != pFlowData->size ())
            {
              updateOffsets (sMode);
              retval = pFlowData->allocate (ssize);
              retval = pFlowData->initialize (timeCurr);
              pState = gridState_t::INITIALIZED;
            }
        }
      if ((!controlFlags[dense_solver]) && (opFlags[jacobian_count_change_flag]))
        {
          if ((retval = pFlowData->sparseReInit (solverInterface::sparse_reinit_modes::resize)) != FUNCTION_EXECUTION_SUCCESS)
            {
              return retval;
            }
        }
    }
  opFlags &= RESET_CHANGE_FLAG_MASK;
  return retval;
}

//we initialize all the objects in the simulation and the default solverInterface
//all other solver data objects would be initialized by a reInitPFlow(xxx) call;
int gridDynSimulation::pFlowInitialize (double time0)
{
  if (time0 == kNullVal)
    {
      time0 = powerFlowStartTime;
      if (time0 == kNullVal)
        {
          time0 = startTime - 0.001;
        }
    }
  LOG_NORMAL ("Initializing Power flow to time " + std::to_string (time0));
  //run any events up to time0
  EvQ->executeEvents (time0 - 0.001);

  
  auto pFlowData = getSolverInterface (*defPowerFlowMode);
  const solverMode &sm = pFlowData->getSolverMode();
  defPowerFlowMode = &sm;
  // initializeB
  //this->savePowerFlowXML("testflow.xml");
  //check the network to ensure we have a solvable power flow

  busCount = getInt ("totalbuscount");
  linkCount = getInt ("totallinkcount");
  timeCurr = time0;
  pFlowInitializeA (time0, lower_flags (controlFlags));
  auto ssize = stateSize (sm);
  pFlowData->allocate (ssize);

  //initialization is divided into two parts to account for complex initialization routines that need to communicate
  //so the A phase is start up and the B phase is to set up the results if needed
  int retval = checkNetwork (network_check_type::full);
  if (retval != FUNCTION_EXECUTION_SUCCESS)
    {
      return retval;
    }
  updateOffsets (sm);
  pFlowObjectInitializeB ();


  if (ssize > 0)
    {
      retval = pFlowData->initialize (time0);
    }
  timeCurr = time0;
  pState = gridState_t::INITIALIZED;
  return FUNCTION_EXECUTION_SUCCESS;
}

//TODO::PT  this really should be done by areas instead of globally
bool gridDynSimulation::loadBalance (double prevPower, const std::vector<double> &prevSlkGen)
{
  double cPower = 0;
  double availPower = 0;
  std::vector<double> avail;
  avail.reserve (m_Buses.size ());
  auto pv = prevSlkGen.begin ();
  for (auto &bus : slkBusses)
    {
      cPower += bus->getGenerationReal ();
      bus->set ("p", *pv);           //reset the slk generators to previous levels so the adjustments work properly
      ++pv;
    }

  cPower = -(cPower - prevPower);

  //printf ("cPower=%f\n", cPower);
  if (std::abs (cPower) < powerAdjustThreshold)
    {
      //just let the residual error go to the swing bus.
      return false;
    }
  //TODO:: PT  this makes a really big assumptions about the location (in the simulation) of the buses really need to put some thought into working this through the areas
  if (cPower > 0)
    {

      for (auto &bus : m_Buses)
        {

          if ((bus->enabled) && (bus->getAdjustableCapacityUp () > 0))
            {
			  double maxGen = bus->getMaxGenReal();
			  double participation = bus->get("participation");
			  if (maxGen > kBigNum / 2)
			  {
				  maxGen = -(1.0 + participation)*(bus->getGenerationReal());
			  }
			  avail.push_back(maxGen);

            }
          else
            {
              avail.push_back (0.0);
            }

        }
    }
  else
    {
      for (auto &bus :m_Buses)
        {
          if ((bus->enabled) && (bus->getAdjustableCapacityDown () > 0))
            {
			  double maxGen = bus->getMaxGenReal();
			  double participation = bus->get("participation");
			  if (maxGen > kBigNum / 2)
			  {
				  maxGen = -(1.0 + participation)*(bus->getGenerationReal());
			  }
			  avail.push_back(maxGen);
            }
          else
            {
              avail.push_back (0.0);
            }

        }
    }

  availPower = sum (avail);
  for (size_t kk = 0; kk < m_Buses.size (); ++kk)
    {
      if (avail[kk] > 0.0)
        {
          m_Buses[kk]->powerAdjust (avail[kk] / availPower * cPower);
        }

    }
  return true;
}


void gridDynSimulation::contingencyAnalysis (contingency_mode_t mode)
{
#ifdef USE_THREADS
#else
  //no threads
  switch (mode)
    {
    case contingency_mode_t::N_1:

      break;
    case contingency_mode_t::N_1_1:
      break;
    case contingency_mode_t::N_2:
      break;
    }
#endif

}

void gridDynSimulation::continuationPowerFlow (const std::string &contName)
{
  std::shared_ptr<continuationSequence> cS;
  for (auto &clN : continList)
    {
      if (contName == clN->name)
        {
          cS = clN;
        }
    }

  if (!cS)
    {
      return;
    }

}

void gridDynSimulation::pFlowSensitivityAnalysis ()
{

}


int gridDynSimulation::eventDrivenPowerflow ( double t_end, double t_step)
{
  if (t_end == kNullVal)
    {
      t_end = stopTime;
    }

  if (t_step == kNullVal)
    {
      t_step = stepTime;
    }
  if (opFlags[dyn_initialized] == false)
    {
      dynInitialize (timeCurr);
    }
  auto ret = EvQ->executeEvents (timeCurr);
  int pret;
  if (ret != change_code::no_change)
    {
      pret = powerflow ();
      if (pret != FUNCTION_EXECUTION_SUCCESS)
        {
          return pret;
        }
    }
  //setup the periodic empty event in the queue
  EvQ->nullEventTime (timeCurr + t_step, t_step);
  double nextEvent = EvQ->getNextTime ();
  bool powerflow_executed;
  while (nextEvent <= t_end - kSmallTime)
    {
      powerflow_executed = false;
      timeCurr = nextEvent;
      //advance the time
      timestep (timeCurr, *defPowerFlowMode);
      //execute any events
      ret = EvQ->executeEventsAonly (timeCurr);
      //run the power flow
      if ((ret >= change_code::parameter_change) || (controlFlags[force_power_flow])|| (EvQ->getNullEventTime () >= timeCurr + t_step))
        {
          pret = powerflow ();
          powerflow_executed = true;
          if (pret != FUNCTION_EXECUTION_SUCCESS)
            {
              return pret;
            }
        }

      //execute delayed events (typically recorders
      ret = EvQ->executeEventsBonly (timeCurr);
      //if something changed rerun the power flow to get a good solution
      //NOTE this would be an atypical situation to have to rerun this
      if (ret >= change_code::parameter_change)
        {
          pret = powerflow ();
          if (pret != FUNCTION_EXECUTION_SUCCESS)
            {
              return pret;
            }
        }
      nextEvent = EvQ->getNextTime ();
      if (powerflow_executed)
        {
          if (nextEvent < timeCurr + t_step)             //only run the empty event if there is nothing in between
            {
              EvQ->nullEventTime (nextEvent + t_step);
            }
        }
    }
  //if necessary do one last power flow this should only happen rarely
  if (timeCurr < t_end - kSmallTime)
    {
      timestep (t_end, cLocalSolverMode);
      pret = powerflow ();
      if (pret != FUNCTION_EXECUTION_SUCCESS)
        {
          return pret;
        }
    }
  timeCurr = t_end;
  return FUNCTION_EXECUTION_SUCCESS;
}

int gridDynSimulation::algUpdateFunction (double ttime, const double state[], double update[], const solverMode &sMode, double alpha)
{
  ++evalCount;
  stateData sD(ttime,state);
  sD.seqID = (sMode.approx[force_recalc] ? 0 : evalCount);

#if (CHECK_STATE > 0)
  auto dynDataa = getSolverInterface (sMode);
  for (size_t kk = 0; kk < dynDataa->getSize (); ++kk)
    {
      if (!std::isfinite (state[kk]))
        {
          LOG_ERROR ("state[" + std::to_string (kk) + "] is not finite");
          return FUNCTION_EXECUTION_FAILURE;
        }
    }
#endif

  if ((!(isDAE (sMode))) && (isDynamic (sMode)))
    {
      sD.fullState = solverInterfaces[defDAEMode->offsetIndex]->state_data ();
    }
  //call the area based function to handle the looping
  preEx (&sD, sMode);
  algebraicUpdate (&sD, update, sMode, alpha);
  delayedAlgebraicUpdate (&sD, update, sMode, alpha);
  return FUNCTION_EXECUTION_SUCCESS;
}

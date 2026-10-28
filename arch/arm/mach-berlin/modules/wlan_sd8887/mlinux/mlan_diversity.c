/** @file mlan_diversity.c
 *
 *  @brief This file contains functions for Antenna Diversity.
 *
 *  (C) Copyright 2008-2016 Marvell International Ltd. All Rights Reserved
 *
 *  MARVELL CONFIDENTIAL
 *  The source code contained or described herein and all documents related to
 *  the source code ("Material") are owned by Marvell International Ltd or its
 *  suppliers or licensors. Title to the Material remains with Marvell
 *  International Ltd or its suppliers and licensors. The Material contains
 *  trade secrets and proprietary and confidential information of Marvell or its
 *  suppliers and licensors. The Material is protected by worldwide copyright
 *  and trade secret laws and treaty provisions. No part of the Material may be
 *  used, copied, reproduced, modified, published, uploaded, posted,
 *  transmitted, distributed, or disclosed in any way without Marvell's prior
 *  express written permission.
 *
 *  No license under any patent, copyright, trade secret or other intellectual
 *  property right is granted to or conferred upon you by disclosure or delivery
 *  of the Materials, either expressly, by implication, inducement, estoppel or
 *  otherwise. Any license under such intellectual property rights must be
 *  express and approved by Marvell in writing.
 *
 */

#include "mlan_diversity.h"
#include "moal_main.h"


#define INITSTATE_0     1
#define INITSTATE_1     2
#define MONITOR_STATE   3
#define SEL_STATE_0     4
#define SEL_STATE_1     5

#define HIGH_RSSI_TH            -62
#define HIGH_RSSI_CHANGE_TH      6   /* For MCS7 -67dBm is the rx sensitivity*/

#define MID_RSSI_TH             -70  /* For 54M -70dBm is the rx sensitivity*/
#define MID_RSSI_CHANGE_TH       4

#define LOW_RSSI_TH             -77 /* For MCS0 -84dBm is the rx sensitivity*/
#define LOW_RSSI_CHANGE_TH       3

#define VERY_LOW_RSSI_TH        -95  /* For 1M -95dBm is the rx sensitivity*/
#define VERY_LOW_RSSI_CHANGE_TH  2

#define PIN_MUX_SWCONFIG_REG    ((volatile t_u32*)0xF7E80408)
#define PIN_MUX_DIRCONFIG_REG   ((volatile t_u32*)0xF7E80404)
#define PIN_MUX_VALUE_REG       ((volatile t_u32*)0xF7E80400)

//Gpio9 is used in Revision 2.1 of the module
#define PIN_MUX_MODECONFIG_REG_2_1  ((volatile t_u32*)0xF7EA800C)
#define ANT_DIV_CTRL_GPIO_2_1 (9)
#define PIN_MUX_MODE_BIT_2_1  (9)

//Gpio14 is used in Revision 2.2 of the module
#define PIN_MUX_MODECONFIG_REG_2_2  ((volatile t_u32*)0xF7EA8000)
#define ANT_DIV_CTRL_GPIO_2_2 (14)
#define PIN_MUX_MODE_BIT_2_2  (6)

inline t_bool did_rssi_drop(t_s32 storedrssi, t_s32 rssi )
{
   if(storedrssi >= HIGH_RSSI_TH)
   {
    //PRINTM(MMSG,"rssi high \n");
   	return ((storedrssi - rssi) > HIGH_RSSI_CHANGE_TH);
   }
   else if(storedrssi >= MID_RSSI_TH)
   {
     //PRINTM(MMSG,"rssi mid \n");
	 return ((storedrssi - rssi) > MID_RSSI_CHANGE_TH );   
   }   
   else if(storedrssi >= LOW_RSSI_TH)
   {
     //PRINTM(MMSG,"rssi low \n");
	 return ((storedrssi - rssi) > LOW_RSSI_CHANGE_TH );
   }   
   else
   {
     //PRINTM(MMSG,"rssi very low \n");
	 return ((storedrssi - rssi) > VERY_LOW_RSSI_CHANGE_TH );
   }
   return 0;
}


#ifdef ANT_DIVERSITY
extern int module_rev;
t_u32 divctrlgpio = ANT_DIV_CTRL_GPIO_2_1;
#endif

void handle_select_antenna(pdiversityctx divdata);
void handle_initial_state(pdiversityctx divdata);
void handle_monitor_state(pdiversityctx pdivdata);
void handle_diversity_states(pdiversityctx pdivdata );
void defer_diversity_action(moal_private *priv, int time);
t_u32 calculate_per(pdiversityctx divdata, t_u32 *rxpackets, t_u32 *rxpktsdelta );

void woal_diversity_timer_exp_handle(void *context )
{
  pdiversityctx pdivdata = (pdiversityctx)context;
  schedule_work(&pdivdata->div_work);
}

void handle_diversity_states(pdiversityctx pdivdata )
{
  moal_private *priv = pdivdata->pmpriv;
  

  if (priv && priv->media_connected == MFALSE)
  {
    PRINTM(MMSG, "Not connected: do not handle diversity\n");
  	return;
  }
  switch(pdivdata->mdivstate)
  {
    case INITSTATE_0:
    case INITSTATE_1:
       handle_initial_state(pdivdata);
       break;
    case MONITOR_STATE:
       handle_monitor_state(pdivdata);
       break;
    case SEL_STATE_0:
    case SEL_STATE_1:
       handle_select_antenna(pdivdata);
       break;
  }
}


int  get_current_rssi(moal_private *priv, t_s16 *prssi)
{
  mlan_ds_get_signal signal;
  memset( &signal, 0, sizeof(mlan_ds_get_signal));

  if (priv->media_connected == MFALSE)
    return MLAN_STATUS_FAILURE;

  if (MLAN_STATUS_SUCCESS != woal_get_signal_info(priv, MOAL_IOCTL_WAIT, &signal)) {
    PRINTM(MERROR, "Error getting signal information\n");
    return MLAN_STATUS_FAILURE;	
  }
  *prssi = signal.bcn_rssi_avg;
//  PRINTM(MMSG, "Rssi val is : %d\n", *prssi );
  return MLAN_STATUS_SUCCESS;
}


int get_current_rx_stat(moal_private *priv, t_u32 *rxpackets )
{
  mlan_ds_get_stats stats;

  memset(&stats, 0, sizeof(mlan_ds_get_stats));

  if (MLAN_STATUS_SUCCESS != woal_get_stats_info(priv, MOAL_IOCTL_WAIT, &stats)) {
    return MLAN_STATUS_FAILURE;
  }
  rxpackets[0] = stats.rx_frag;
  rxpackets[1] = stats.fcs_error;  

  PRINTM(MDATA, "stats.rx_frag : %d stats.fcs_error: %d \n", stats.rx_frag, stats.fcs_error );
  return MLAN_STATUS_SUCCESS;
}


void configure_ant_diversityctrl(void)
{
  volatile t_u32* gpioreg = PIN_MUX_SWCONFIG_REG;
  t_u32 regval, modebit, gpiono;  

  *gpioreg = 0;

  PRINTM(MMSG, "module_rev: %d \n", module_rev);
  
  if(module_rev == 21)
  {
    gpioreg = PIN_MUX_MODECONFIG_REG_2_1;
	modebit = PIN_MUX_MODE_BIT_2_1;
	gpiono =  ANT_DIV_CTRL_GPIO_2_1;
	divctrlgpio = ANT_DIV_CTRL_GPIO_2_1;
  }
  else
  {
    gpioreg = PIN_MUX_MODECONFIG_REG_2_2;
    modebit = PIN_MUX_MODE_BIT_2_2; 
	gpiono =  ANT_DIV_CTRL_GPIO_2_2;
	divctrlgpio = ANT_DIV_CTRL_GPIO_2_2;
  }

  PRINTM(MMSG, "gpioreg: %x modebit: %d gpiono: %d\n", gpioreg, modebit, gpiono );
  
  regval = *gpioreg;
  regval = (regval & ~( (t_u32)7 << modebit))+( (t_u32)1 << modebit);
  *gpioreg = regval;
  gpioreg = PIN_MUX_DIRCONFIG_REG;
  regval = *gpioreg;
  regval = regval | ( (t_u32)1 << gpiono );
  *gpioreg = regval;
}

void set_antenna_sel(int ant)
{
  volatile t_u32* gpioreg = PIN_MUX_VALUE_REG;
  t_u32 regval;

  if (ant == ANTENNA_0)
  {
    regval = *gpioreg;
    *gpioreg = regval & ~( (t_u32)1 << divctrlgpio );
  }
  else if (ant == ANTENNA_1)
  {			  
    regval = *gpioreg;
    *gpioreg = regval | ( (t_u32)1 << divctrlgpio );
  }  
  PRINTM(MMSG, "Selected Antenna : %d\n", ant);
}

t_void
woal_div_work_queue(struct delayed_work *work)
{
  pdiversityctx pdivdata = container_of(work, diversityctx, div_work.work);
  handle_diversity_states(pdivdata);
}


void init_antenna_settings(moal_private *priv)
{
  pdiversityctx pdivdata = &priv->divdata;
  PRINTM(MMSG, "init_antenna_settings: %x\n", pdivdata );
  memset(pdivdata, 0, sizeof(diversityctx));
  pdivdata->currantenna = ANTENNA_0;
  pdivdata->mdivstate = INITSTATE_0;
  pdivdata->pmpriv = priv;
  configure_ant_diversityctrl();
  set_antenna_sel(ANTENNA_0);
 // woal_initialize_timer(&pdivdata->diversity_tmr, woal_diversity_timer_exp_handle, pdivdata );
  MLAN_INIT_DELAYED_WORK(&pdivdata->div_work, woal_div_work_queue);
 // pdivdata->divworkqueue = alloc_workqueue("DIV_WORK_QUEUE", WQ_HIGHPRI | WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
}

void start_diversity_action(moal_private *priv)
{
  pdiversityctx pdivdata = &priv->divdata;  
  pdivdata->currantenna = ANTENNA_0;
  pdivdata->mdivstate = INITSTATE_0;  
  set_antenna_sel(ANTENNA_0);
  PRINTM(MMSG, "start_diversity_action\n" );
  defer_diversity_action(priv, 500);
}

void stop_diversity_action(moal_private *priv)
{
  pdiversityctx pdivdata = &priv->divdata;  
  PRINTM(MMSG, "stop_diversity_action\n" );
  cancel_delayed_work_sync(&pdivdata->div_work);  
}


void defer_diversity_action(moal_private *priv, int time)
{  
  pdiversityctx pdivdata = &priv->divdata;  
  //PRINTM(MMSG, "defer_diversity_action\n" );  
  schedule_delayed_work(&pdivdata->div_work, msecs_to_jiffies(time));
}

void handle_initial_state(pdiversityctx pdivdata)
{
  t_s16 rssi;
  t_u32 rxpackets[2];
  int ret;

  PRINTM(MMSG, "handle_initial_state\n" );

  if (pdivdata->mdivstate == INITSTATE_0)
  {
    ret = get_current_rssi(pdivdata->pmpriv, &rssi);
    if(ret == MLAN_STATUS_SUCCESS)
    {
      PRINTM(MMSG, "state : %d rssi : %d\n", pdivdata->mdivstate, rssi);
      pdivdata->currrssi = rssi;
      set_antenna_sel(ANTENNA_1);
      pdivdata->currantenna = ANTENNA_1;
      pdivdata->mdivstate = INITSTATE_1;	  
    }
	defer_diversity_action(pdivdata->pmpriv, 700);
  }
  else if (pdivdata->mdivstate == INITSTATE_1)
  {
    ret = get_current_rssi(pdivdata->pmpriv, &rssi);
    if(ret == MLAN_STATUS_SUCCESS)
    {
      PRINTM(MMSG, "state : %d rssi : %d\n", pdivdata->mdivstate, rssi);
      if ( rssi < pdivdata->currrssi)
      {
        set_antenna_sel(ANTENNA_0);
        pdivdata->currantenna = ANTENNA_0;		  
      }
      else
        pdivdata->currrssi = rssi;        
    }
    ret = get_current_rx_stat(pdivdata->pmpriv, rxpackets );
    if(ret == MLAN_STATUS_SUCCESS)
    {
      pdivdata->totalpkts    =  rxpackets[0];
      pdivdata->fcserrorpkts =  rxpackets[1];
    }
    pdivdata->mdivstate = MONITOR_STATE;
    defer_diversity_action(pdivdata->pmpriv, 1500);
  }  
}


void handle_monitor_state(pdiversityctx pdivdata)
{
  t_u32 rxpackets[2];
  t_s32 per, rxpktsdelta = 0;
  int ret;
  t_s16 rssi;
  
  ret = get_current_rssi(pdivdata->pmpriv, &rssi);

  PRINTM(MDATA, "MON rssi: %d currrssi: %d", pdivdata->currrssi, rssi);

  if(ret == MLAN_STATUS_SUCCESS )
  {    
    if ( rssi < pdivdata->currrssi && did_rssi_drop(pdivdata->currrssi, rssi) )
    {
	  PRINTM(MMSG, "RSSI dropped\n");       
      pdivdata->mdivstate = SEL_STATE_0;
      pdivdata->currrssi = rssi;
      handle_select_antenna(pdivdata);
    }
	else if(rssi > pdivdata->currrssi)
      pdivdata->currrssi = rssi;
  }
  
  ret = get_current_rx_stat(pdivdata->pmpriv, rxpackets );
  
  if(ret == MLAN_STATUS_SUCCESS)
  {
	per = calculate_per(pdivdata, rxpackets, &rxpktsdelta );
	pdivdata->totalpkts	 =	rxpackets[0];
	pdivdata->fcserrorpkts =	rxpackets[1];
	if (per >=0 )
	{
	  if( rxpktsdelta > 1000 && per > 50 )
	  {		 
		pdivdata->curper = per;
		pdivdata->mdivstate = SEL_STATE_0;
		handle_select_antenna(pdivdata);
	  }
	  else if(rxpktsdelta > 1000)
		pdivdata->curper = per;
	}
  } 
  
  ret = get_current_rx_stat(pdivdata->pmpriv, rxpackets );
  if(ret == MLAN_STATUS_SUCCESS)
  {
    pdivdata->totalpkts    =  rxpackets[0];
    pdivdata->fcserrorpkts =  rxpackets[1];
  }

  if(pdivdata->mdivstate == MONITOR_STATE)
    defer_diversity_action(pdivdata->pmpriv, 1500);
}

void handle_select_antenna(pdiversityctx pdivdata)
{
  t_u32 rxpackets[2];
  t_s32 per, rxpktsdelta = 0;
  int ret, otherant;
  t_s16 rssi;

  PRINTM(MDATA, "handle_select_antenna : %d\n", pdivdata->mdivstate );
  
  otherant = (pdivdata->currantenna == ANTENNA_0 ? ANTENNA_1: ANTENNA_0 );

  if(pdivdata->mdivstate == SEL_STATE_0)
  {    
    set_antenna_sel(otherant);
    pdivdata->currantenna = otherant;
    pdivdata->mdivstate = SEL_STATE_1;
    defer_diversity_action(pdivdata->pmpriv, 700);
  }
  else if(pdivdata->mdivstate == SEL_STATE_1)
  {
    get_current_rx_stat(pdivdata->pmpriv, rxpackets );
    pdivdata->totalpkts    =  rxpackets[0];
    pdivdata->fcserrorpkts =  rxpackets[1];
    per = calculate_per(pdivdata, rxpackets, &rxpktsdelta);  
    ret = get_current_rssi(pdivdata->pmpriv, &rssi);

	PRINTM(MDATA, "SEL rssi : %d currrssi : %d\n", rssi, pdivdata->currrssi);

    if( per > (pdivdata->curper + 5) || rssi < pdivdata->currrssi)
    {
      set_antenna_sel(otherant);
      pdivdata->currantenna = otherant;
    }
    else
    {
      pdivdata->currrssi = rssi;
      pdivdata->curper = per;
    }	
    pdivdata->mdivstate = MONITOR_STATE;
    defer_diversity_action(pdivdata->pmpriv, 1500);
  }   
}


t_u32 calculate_per(pdiversityctx pdivdata, t_u32 *rxpackets,  t_u32 *rxpktsdelta)
{
  t_u32 rxpktdelta = 0, fcspktdelta = 0;
  t_u32 per = 0;

  if(rxpackets[0] > pdivdata->totalpkts)
    rxpktdelta = rxpackets[0] - pdivdata->totalpkts;
  if(rxpackets[1] > pdivdata->fcserrorpkts )
    fcspktdelta = rxpackets[1] - pdivdata->fcserrorpkts;

  if( rxpktdelta && fcspktdelta)
    per = (fcspktdelta*100)/(rxpktdelta+fcspktdelta);

  *rxpktsdelta = rxpktdelta;
  PRINTM(MDATA, "rxpktdelta : %d fcspktdelta : %d per : %d \n", rxpktdelta, fcspktdelta, per );

  return per;
}  




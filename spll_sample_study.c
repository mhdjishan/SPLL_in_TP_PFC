#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"
#include "stdio.h"
#include "math.h"
#include "spll_1ph_sogi.h"

// --- CONSTANTS ---
#define GRID_FREQ           50.0f
#define SLOW_ISR_FREQ       10000.0f  // 10kHz for Voltage Loop and PLL
#define FAST_ISR_FREQ       50000.0f  // 50kHz for Current Loop

#define PWM_PERIOD          2000      // 100MHz / 50kHz / 1 = 2000 (Up-count)
#define ADC_OFFSET          2048.0f   // AC sense signal offset at zero AC voltage

// PWM Scheduler & Blanking
#define DEAD_BAND_TICKS     40        // 400ns dead-band (40 cycles * 10ns)
#define BLANKING_ANGLE_RAD  0.157f    // Phase angle for 500us blanking at 50Hz
#define PI_VAL              3.14159265f
#define TWO_PI_VAL          6.2831853f

//limits
#define MAX_DUTY 0.95f
#define MIN_DUTY 0.01f

// --- SCALING FACTORS ---
const float ADC_SCALE_AC  = 339.0f / 2048.0f;
const float ADC_SCALE_DC  = 497.0f / 4096.0f;
const float ADC_SCALE_L_I = 9.972f / 2048.0f;

// --- GLOBALS ---
SPLL_1PH_SOGI spll1;

volatile uint16_t currentDuty = 400; 
volatile bool pwm_flag = false;

// RMS & DC Extraction
volatile float final_rms = 0.0f;
volatile float final_dc_voltage = 0.0f;
volatile float final_i_rms = 0.0f;

// ADC timer_isr variables
volatile float temp_sum_squares = 0.0f;    // Changed to float to avoid casting in ISR
volatile float temp_sum_squares_i = 0.0f;
volatile float sum_squares_ac = 0.0f;      // Changed to float
volatile float sum_squares_i_ac = 0.0f;

volatile uint16_t sample_counter = 0;
volatile uint16_t flatline_counter = 0;
volatile bool data_ready_flag = false;

volatile float ac_vol_adc_offset = 2084.0f; 

volatile float ac_vol_normalized = 0.0f;
volatile float v_dc_meas = 0.0f;
volatile float i_ac_meas = 0.0f;

// Control Loop Variables
volatile float v_dc_ref = 380.0f;      // Target DC Bus Voltage
volatile float i_ref_amplitude = 0.0f; // Output of Voltage Loop
volatile float i_ref_inst = 0.0f;      // Instantaneous current reference
volatile float currentDutyFloat = 0.0f;

// --- FUNCTION PROTOTYPES ---
void initEPWM_HFL(void);
void initEPWM_LFL(void);
void initADCA(void);
void initADCB(void);
void initADCC(void);
void initSCIA(void);
void sendSCIText(char *msg);

__interrupt void fastCurrentLoop_ISR(void);
__interrupt void slowVoltageLoop_ISR(void);

void main(void)
{
    Device_init();
    Device_initGPIO();
    Interrupt_initModule();
    Interrupt_initVectorTable();
    
    Interrupt_register(INT_ADCA1, &fastCurrentLoop_ISR); // 50kHz
    Interrupt_register(INT_ADCC1, &slowVoltageLoop_ISR); // 10kHz

    Board_init();
    initEPWM_HFL();
    initEPWM_LFL();
    
    // Initialize ADCs
    initADCA();
    initADCB();
    initADCC();
    DEVICE_DELAY_US(1000); // Consolidated ADC delay (only need to wait once for all 3)

    initSCIA();

    SPLL_1PH_SOGI_reset(&spll1);
    SPLL_1PH_SOGI_config(&spll1, GRID_FREQ, FAST_ISR_FREQ, 166.9743f, -166.2124f);
    SPLL_1PH_SOGI_coeff_calc(&spll1);

    C2000Ware_libraries_init();
    
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    
    EINT;
    ERTM;

    // Force PWM low initially
    EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST); 
    EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    
    char buffer2[100];
    
    while(1)
    {   
        if(data_ready_flag)
        {
            data_ready_flag = false;
            
            // Calculate RMS
            final_rms = sqrtf(sum_squares_ac * 0.001f) * ADC_SCALE_AC;
            final_i_rms = sqrtf(sum_squares_i_ac * 0.001f);
            int current_mA = (int)(final_i_rms * 1000.0f);
            final_dc_voltage = v_dc_meas;

            // State Machine / Safety Logic
            if(final_rms >= 60.0f && final_dc_voltage < 300.0f)
            {
                pwm_flag = true;
            }
            else if (final_rms < 40.0f || final_dc_voltage >= 380.0f)
            {
                pwm_flag = false;
            }
            
            sprintf(buffer2, " DC_voltage : %d | Sin RMS: %d | Current: %d mA | duty : %u \r\n", 
                    (int)final_dc_voltage, (int)final_rms, current_mA, currentDuty);
            sendSCIText(buffer2);
        }
    }
}

// SLOW LOOP: 10kHz (Triggered by EPWM6 SOCB prescaled by 5)
__interrupt void slowVoltageLoop_ISR(void)
{
    uint16_t raw_v_dc = ADC_readResult(ADCCRESULT_BASE, ADC_SOC_NUMBER0);
    v_dc_meas = (float)raw_v_dc * ADC_SCALE_DC;
    
    // Voltage Loop PI Controller goes here...
       
    ADC_clearInterruptStatus(ADCC_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

// FAST LOOP: 50kHz (Triggered by EPWM6 SOCA prescaled by 1)

__interrupt void fastCurrentLoop_ISR(void)
{
    uint16_t raw_i_ac = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0);
    uint16_t raw_v_ac = ADC_readResult(ADCBRESULT_BASE, ADC_SOC_NUMBER0);

    // Update global normalized voltage
    ac_vol_normalized = ((float)raw_v_ac - ADC_OFFSET) / ADC_OFFSET;

    SPLL_1PH_SOGI_run(&spll1, ac_vol_normalized); 

    i_ac_meas = ((float)raw_i_ac - ADC_OFFSET) * ADC_SCALE_L_I;
    i_ref_inst = i_ref_amplitude * spll1.sine; 

    //starting of close loop feed forward term
    float peak_grid_vol = final_rms * 1.4142f; 
    float abs_v_ac_inst = fabsf(peak_grid_vol * spll1.sine);

     float duty_feedforward = 0.0f;
    if (v_dc_meas > 10.0f) { 
        duty_feedforward = 1.0f - (abs_v_ac_inst / v_dc_meas);     // D = 1 - (Vin/Vout)
    }

    currentDutyFloat = duty_feedforward ;
    if (currentDutyFloat > MAX_DUTY) {
        currentDutyFloat = MAX_DUTY;
    } else if (currentDutyFloat < MIN_DUTY) {
        currentDutyFloat = MIN_DUTY;
    }

    currentDuty = (uint16_t)(currentDutyFloat * PWM_PERIOD);

    float theta = spll1.theta; // 0 to 2*PI
    // Zero-Crossing Blanking Logic
    bool in_blanking_window = (theta < BLANKING_ANGLE_RAD) || 
                              (theta > (PI_VAL - BLANKING_ANGLE_RAD) && theta < (PI_VAL + BLANKING_ANGLE_RAD)) ||
                              (theta > (TWO_PI_VAL - BLANKING_ANGLE_RAD));

    if (pwm_flag && !in_blanking_window) 
    {
        if (theta < PI_VAL) {
            // Positive Half Cycle
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_HIGH); 
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_B, EPWM_AQ_SW_OUTPUT_LOW);
        } else {
            // Negative Half Cycle
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, PWM_PERIOD - currentDuty);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_LOW);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_B, EPWM_AQ_SW_OUTPUT_HIGH);
        }

        EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);
        EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);
    } 
    else 
    {
        EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
        EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    }
    // Flatline Check
    if(raw_v_ac <= 150) {
        flatline_counter++;
    }

    // RMS Accumulation
    float adj_f = (float)raw_v_ac - ac_vol_adc_offset;
    temp_sum_squares += (adj_f * adj_f);
    temp_sum_squares_i += (i_ac_meas * i_ac_meas);
    
    sample_counter++;
    if(sample_counter >= 1000)
    {
        if (flatline_counter < 480)
        {
            sum_squares_ac = temp_sum_squares;
            sum_squares_i_ac = temp_sum_squares_i; 
        }
        else
        {
            sum_squares_ac = 0.0f;
            sum_squares_i_ac = 0.0f;
        }

        sample_counter = 0;
        flatline_counter = 0;
        temp_sum_squares = 0.0f;
        temp_sum_squares_i = 0.0f; 
        data_ready_flag = true;
    }

    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}


//adc initializationfor output and input
void initADCA(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_ADCA);
    ADC_setVREF(ADCA_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCA_BASE, ADC_CLK_DIV_4_0);
    ADC_enableConverter(ADCA_BASE);

    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN6, 15); 
    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER0);
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER0);
    Interrupt_enable(INT_ADCA1);
}

void initADCB(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_ADCB);
    ADC_setVREF(ADCB_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCB_BASE, ADC_CLK_DIV_4_0);
    ADC_enableConverter(ADCB_BASE);

    ADC_setupSOC(ADCB_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN2, 15);
    ADC_forceSOC(ADCB_BASE, ADC_SOC_NUMBER0);
}

void initADCC(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_ADCC);
    ADC_setVREF(ADCC_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCC_BASE, ADC_CLK_DIV_4_0);
    ADC_enableConverter(ADCC_BASE);

    ADC_setupSOC(ADCC_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_EPWM6_SOCB, ADC_CH_ADCIN2, 15);
    ADC_setInterruptSource(ADCC_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER0);
    ADC_enableInterrupt(ADCC_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCC_BASE, ADC_INT_NUMBER1);
    
    ADC_forceSOC(ADCC_BASE, ADC_SOC_NUMBER0);
    Interrupt_enable(INT_ADCC1);
}

// for serial communication UART
void initSCIA(void)
{
    GPIO_setPinConfig(GPIO_28_SCIA_RX);
    GPIO_setPinConfig(GPIO_29_SCIA_TX);
    GPIO_setDirectionMode(28, GPIO_DIR_MODE_IN);
    GPIO_setPadConfig(28, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(28, GPIO_QUAL_ASYNC);
    GPIO_setDirectionMode(29, GPIO_DIR_MODE_OUT);
    GPIO_setPadConfig(29, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(29, GPIO_QUAL_ASYNC);
    SCI_setConfig(SCIA_BASE, DEVICE_LSPCLK_FREQ, 115200, SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE);
    SCI_enableFIFO(SCIA_BASE);
    SCI_resetChannels(SCIA_BASE);  
    SCI_resetRxFIFO(SCIA_BASE);
    SCI_resetTxFIFO(SCIA_BASE);
    SCI_enableModule(SCIA_BASE);
    SCI_enableTxModule(SCIA_BASE);
    SCI_enableRxModule(SCIA_BASE);
}
//for serial communication to print
void sendSCIText(char *msg)
{
    while (*msg)
    {
        SCI_writeCharBlockingFIFO(SCIA_BASE, *msg++);
    }
}


void initEPWM_HFL(void)
{
    GPIO_setPinConfig(GPIO_10_EPWM6A);
    GPIO_setPadConfig(10, GPIO_PIN_TYPE_STD);
    GPIO_setPinConfig(GPIO_11_EPWM6B);
    GPIO_setPadConfig(11, GPIO_PIN_TYPE_STD);

    EPWM_setClockPrescaler(EPWM6_BASE, EPWM_CLOCK_DIVIDER_1, EPWM_HSCLOCK_DIVIDER_1);
    EPWM_setTimeBasePeriod(EPWM6_BASE, PWM_PERIOD);
    EPWM_setTimeBaseCounterMode(EPWM6_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setPhaseShift(EPWM6_BASE, 0);
   
    EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);
    EPWM_setCounterCompareShadowLoadMode(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, EPWM_COMP_LOAD_ON_CNTR_ZERO);

    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);
    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);

    EPWM_setDeadBandDelayMode(EPWM6_BASE, EPWM_DB_RED, true);
    EPWM_setDeadBandDelayMode(EPWM6_BASE, EPWM_DB_FED, true);
    EPWM_setRisingEdgeDeadBandDelayInput(EPWM6_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setFallingEdgeDeadBandDelayInput(EPWM6_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setDeadBandDelayPolarity(EPWM6_BASE, EPWM_DB_RED, EPWM_DB_POLARITY_ACTIVE_HIGH);
    EPWM_setDeadBandDelayPolarity(EPWM6_BASE, EPWM_DB_FED, EPWM_DB_POLARITY_ACTIVE_LOW);
    EPWM_setRisingEdgeDelayCount(EPWM6_BASE, DEAD_BAND_TICKS);
    EPWM_setFallingEdgeDelayCount(EPWM6_BASE, DEAD_BAND_TICKS);

    // Set CMPC to the exact midpoint of the PWM period (1000)
    EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_C, 1000);
    
    EPWM_setADCTriggerSource(EPWM6_BASE, EPWM_SOC_A, EPWM_SOC_TBCTR_U_CMPC);
    EPWM_setADCTriggerSource(EPWM6_BASE, EPWM_SOC_B, EPWM_SOC_TBCTR_U_CMPC);
    
    // SOCA triggers every 1 count (50kHz -> Fast Loop)
    EPWM_setADCTriggerEventPrescale(EPWM6_BASE, EPWM_SOC_A, 1);
    // SOCB triggers every 5 counts (50kHz / 5 = 10kHz -> Slow Loop)
    EPWM_setADCTriggerEventPrescale(EPWM6_BASE, EPWM_SOC_B, 5); 

    EPWM_enableADCTrigger(EPWM6_BASE, EPWM_SOC_A);
    EPWM_enableADCTrigger(EPWM6_BASE, EPWM_SOC_B);

    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
}

void initEPWM_LFL(void)
{
    GPIO_setPinConfig(GPIO_8_EPWM5A);
    GPIO_setPadConfig(8, GPIO_PIN_TYPE_STD);
    GPIO_setPinConfig(GPIO_9_EPWM5B);
    GPIO_setPadConfig(9, GPIO_PIN_TYPE_STD);

    EPWM_setTimeBasePeriod(EPWM5_BASE, PWM_PERIOD); 
    EPWM_setTimeBaseCounterMode(EPWM5_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setActionQualifierAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);

    EPWM_setDeadBandDelayMode(EPWM5_BASE, EPWM_DB_RED, true);
    EPWM_setDeadBandDelayMode(EPWM5_BASE, EPWM_DB_FED, true);
    EPWM_setRisingEdgeDeadBandDelayInput(EPWM5_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setFallingEdgeDeadBandDelayInput(EPWM5_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setDeadBandDelayPolarity(EPWM5_BASE, EPWM_DB_RED, EPWM_DB_POLARITY_ACTIVE_HIGH);
    EPWM_setDeadBandDelayPolarity(EPWM5_BASE, EPWM_DB_FED, EPWM_DB_POLARITY_ACTIVE_LOW);
    EPWM_setRisingEdgeDelayCount(EPWM5_BASE, DEAD_BAND_TICKS);
    EPWM_setFallingEdgeDelayCount(EPWM5_BASE, DEAD_BAND_TICKS);

    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);
}

// End of File
//
//what is remaing

////hardware over-current protection mapped to the CMPSS (Comparator Subsystem)
// You Forgot the Auto-Zero Calibration!
// your op-amps are not perfect. Subtracting exactly 2048 will cause your SOGI PLL to vibrate and your current RMS readout to be completely wrong when the board is resting.
//  Please make sure you add the current_adc_offset logic to calculate the true hardware zero-point when the PFC is off! with help of example code
//* find a method to ofset auto correction
//* find a method to make all the sense are ok at the initially INDUSTRIAL PRE-FLIGHT CHECKS and other safty feature like sensor fault etc
// flat line issue wll solve when auto offset adjust is implemented

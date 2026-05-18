#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"
#include "stdio.h"
#include "math.h"
#include "spll_1ph_sogi.h"




// Initialize the PLL object
SPLL_1PH_SOGI spll1;
// Constants for the PLL
#define GRID_FREQ 50.0f
#define ISR_FREQ  10000.0f  // Matching with adcA1_ISR (100us)

#define PWM_PERIOD 1666 //60khz // calculation 100Mhz/required freaquency
#define ADC_OFFSET 2048 // offset is get by measuring the AC sense signal at zero ac voltage ((that point of DC voltage)/3.3)*4096
//for the pwm scheduler
#define dead_band 40 //400 nanoseconds no of cycles * (1/100MHz)

volatile uint16_t currentDuty = 322; //volatile is used since it is modified in interrupt

volatile bool pwm_flag = false;

//for tripping logic

//rms calculation new
// 2048 counts = 325V. Scale = 325/2048 = 0.15869
const float VOLTS_PER_COUNT = 0.15869f;
volatile uint32_t sum_squares_ac =0;
volatile float final_rms = 0.0f;
volatile float final_dc_voltage = 0.0f;

//ADC timer_isr variables
volatile uint32_t temp_sum_squares = 0;
volatile uint16_t sample_counter = 0;

volatile uint32_t temp_sum_raw_dc = 0;
volatile uint32_t sum_raw_dc = 0;

volatile uint16_t raw_cal_dc = 0;
volatile uint16_t flatline_counter = 0;
volatile bool data_ready_flag = false;
const float ADC_SCALE_AC = 325.0f / 2048.0f;
const float ADC_SCALE_DC = 294.0f / 4096.0f;
SPLL_1PH_SOGI spll1;
volatile float ac_vol_normalized = 0.0f;

void initEPWM_HFL(void);
void initEPWM_LFL(void);

void initADC(void);
void initSCIA(void);

void sendSCIText(char *msg);
__interrupt void adcA1_ISR(void);

void main(void)
{
    // Initialize device clock and peripherals
    Device_init();
    // Disable pin locks and enable internal pull-ups.
    Device_initGPIO();
    // Initialize PIE and clear PIE registers. Disables CPU interrupts.
    Interrupt_initModule();
    // Initialize the PIE vector table with pointers to the shell Interrupt
    // Service Routines (ISR).
    Interrupt_initVectorTable();
    Interrupt_register(INT_ADCA1, &adcA1_ISR);

    // PinMux and Peripheral Initialization
    Board_init();
    initEPWM_HFL();
    initEPWM_LFL();

    initADC();
    initSCIA();

    SPLL_1PH_SOGI_reset(&spll1);
    // Typical coefficients for 50Hz grid sampled at 10kHz
    SPLL_1PH_SOGI_config(&spll1, GRID_FREQ, ISR_FREQ, 166.9743f, -166.2124f);
    SPLL_1PH_SOGI_coeff_calc(&spll1);


    // C2000Ware Library initialization
    C2000Ware_libraries_init();
    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    // Enable Global Interrupt (INTM) and real time interrupt (DBGM)
    EINT;
    ERTM;

    EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST); // to force pwm low initially (no pwm at the beginning)
    EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    char buffer2[100];
   
    while(1)
    {
        if(data_ready_flag)
        {
            float mean_square = (float)sum_squares_ac *0.005f;
            float rms_counts = sqrtf(mean_square);
            final_rms = rms_counts * ADC_SCALE_AC;

            float mean_sum_raw_dc = (float)sum_raw_dc *0.005f;
            final_dc_voltage = mean_sum_raw_dc * ADC_SCALE_DC ;

            if(final_rms >= 30.0f && final_dc_voltage < 120.0f)
            {
                pwm_flag = true;
             

            }
            else if (final_rms < 20.0f)
            {
                pwm_flag = false;
                
               
            }
            else if (final_dc_voltage >= 150.0f) 
            {
                pwm_flag = false;
                
            }
            data_ready_flag = false;
            sprintf(buffer2, "DC_BUS_ADC: %d | DC_voltage : %d | Sin RMS: %d | duty : %d \r\n\r\n  ", raw_cal_dc , (int)final_dc_voltage, (int)final_rms , currentDuty);
            sendSCIText(buffer2);
           
        }
       
    }
}

//interrupts that run every 100us that timmer 3
__interrupt void adcA1_ISR(void)
{
        int16_t adj;
    uint16_t raw_ac = 0;
    uint16_t raw_dc = 0 ;
    raw_ac = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0);
    raw_dc = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER1);
    raw_cal_dc = raw_dc;
    //ac adc section
    // Normalize AC input to -1.0 to 1.0 range for the PLL
    // (Assuming 2048 is zero cross and peak is approx 4095)
    ac_vol_normalized = ((float)raw_ac - ADC_OFFSET) / 2048.0f;
    // 2. Run the PLL
    SPLL_1PH_SOGI_run(&spll1, ac_vol_normalized);

    // 3. Extract Phase Information
    // spll1.sine   -> Normalized sine wave in phase with Grid
    // spll1.theta  -> The phase angle (0 to 2*PI)
// ZERO-CROSSING BLANKING LOGIC
    // -----------------------------------------------------------
    // Define the blanking window (e.g., 0.05 = 5% of peak AC voltage)
    // You can increase this value to make the delay longer.
    float zero_cross_threshold = 0.08f; 

    // Check if system is ON *AND* we are OUTSIDE the zero-crossing window
    if (pwm_flag == true && fabsf(spll1.sine) >= zero_cross_threshold) 
    {
        // 1. Clear the trips (Turn the FETs back on)
        EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);
        EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);

        // 2. Apply Normal Modulation
        if (spll1.sine >= 0) {
            // Positive Half
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_HIGH);
        } else {
            // Negative Half
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, PWM_PERIOD - currentDuty);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_LOW);
        }
    } 
    else 
    {
        // BLANKING WINDOW OR SYSTEM OFF
        // Safely force all high-frequency and low-frequency FETs OFF
        EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
        EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    }
    // -----------------------------------------------------------
   
    if(raw_ac <= 150)
    {
        flatline_counter++;
    }

    adj = (int16_t)raw_ac - ADC_OFFSET;
    temp_sum_squares += (uint32_t)((int32_t)adj * (int32_t)adj);
    temp_sum_raw_dc += (uint32_t)raw_cal_dc ;
    sample_counter++;
    if(sample_counter >= 200)
    {
        if (flatline_counter < 180 )
        {
            sum_squares_ac = temp_sum_squares;
            sum_raw_dc = temp_sum_raw_dc ;
        }
        else
        {
            sum_squares_ac = 0;
            sum_raw_dc = temp_sum_raw_dc ;
        }
        sample_counter = 0;
        temp_sum_squares = 0;
        temp_sum_raw_dc = 0;
        flatline_counter = 0;
        data_ready_flag = true;
    }

    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER0);
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

//adc initializationfor output and input
void initADC(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_ADCA);
    ADC_setVREF(ADCA_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCA_BASE, ADC_CLK_DIV_4_0);
    ADC_enableConverter(ADCA_BASE);
    DEVICE_DELAY_US(1000);

// Remove ADC_TRIGGER_SW_ONLY and replace with EPWM6 SOCA
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN6, 15);
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER1, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN5, 15); 

    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER0);
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);

    // Prime the ADC so the first ISR pass has valid data to read
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER0);
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER1);
    Interrupt_enable(INT_ADCA1);
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

    // Enable Shadow Load for CMPA to load on Zero
    EPWM_setCounterCompareShadowLoadMode(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, EPWM_COMP_LOAD_ON_CNTR_ZERO);

    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);
    EPWM_setActionQualifierAction(EPWM6_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);

    EPWM_setDeadBandDelayMode(EPWM6_BASE, EPWM_DB_RED, true);
    EPWM_setDeadBandDelayMode(EPWM6_BASE, EPWM_DB_FED, true);
    EPWM_setRisingEdgeDeadBandDelayInput(EPWM6_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setFallingEdgeDeadBandDelayInput(EPWM6_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setDeadBandDelayPolarity(EPWM6_BASE, EPWM_DB_RED, EPWM_DB_POLARITY_ACTIVE_HIGH);
    EPWM_setDeadBandDelayPolarity(EPWM6_BASE, EPWM_DB_FED, EPWM_DB_POLARITY_ACTIVE_LOW);
    EPWM_setRisingEdgeDelayCount(EPWM6_BASE, dead_band);
    EPWM_setFallingEdgeDelayCount(EPWM6_BASE, dead_band);

    // Add this to initEPWM_HFL()
    EPWM_setADCTriggerSource(EPWM6_BASE, EPWM_SOC_A, EPWM_SOC_TBCTR_ZERO); // Sample at CTR=0
    EPWM_setADCTriggerEventPrescale(EPWM6_BASE, EPWM_SOC_A, 6); // Trigger every 6th PWM pulse (10kHz control loop from 60kHz PWM)
    EPWM_enableADCTrigger(EPWM6_BASE, EPWM_SOC_A);

    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);

}

void initEPWM_LFL(void)
{
    GPIO_setPinConfig(GPIO_8_EPWM5A);
    GPIO_setPadConfig(8, GPIO_PIN_TYPE_STD);
    GPIO_setPinConfig(GPIO_9_EPWM5B);
    GPIO_setPadConfig(9, GPIO_PIN_TYPE_STD);


    // ADD THIS: Give EPWM5 a basic timebase and action qualifier baseline
    EPWM_setTimeBasePeriod(EPWM5_BASE, PWM_PERIOD); 
    EPWM_setTimeBaseCounterMode(EPWM5_BASE, EPWM_COUNTER_MODE_UP);
    EPWM_setActionQualifierAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_ZERO);


    EPWM_setDeadBandDelayMode(EPWM5_BASE, EPWM_DB_RED, true);
    EPWM_setDeadBandDelayMode(EPWM5_BASE, EPWM_DB_FED, true);
    EPWM_setRisingEdgeDeadBandDelayInput(EPWM5_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setFallingEdgeDeadBandDelayInput(EPWM5_BASE, EPWM_DB_INPUT_EPWMA);
    EPWM_setDeadBandDelayPolarity(EPWM5_BASE, EPWM_DB_RED, EPWM_DB_POLARITY_ACTIVE_HIGH);
    EPWM_setDeadBandDelayPolarity(EPWM5_BASE, EPWM_DB_FED, EPWM_DB_POLARITY_ACTIVE_LOW);
    EPWM_setRisingEdgeDelayCount(EPWM5_BASE, dead_band);
    EPWM_setFallingEdgeDelayCount(EPWM5_BASE, dead_band);


    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM5_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
    EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);
}

// End of File
//

#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "c2000ware_libraries.h"
#include "stdio.h"
#include "math.h"
#include "spll_1ph_sogi.h"

// --- CONSTANTS ---
#define GRID_FREQ 50.0f
#define SLOW_ISR_FREQ 10000.0f  // 10kHz for Voltage Loop and PLL
#define FAST_ISR_FREQ 50000.0f  // 50kHz for Current Loop


#define PWM_PERIOD 2000         // 100MHz / 50kHz / 1 = 2000 (Up-count)
#define ADC_OFFSET 2048 // offset is get by measuring the AC sense signal at zero ac voltage ((that point of DC voltage)/3.3)*4096
//for the pwm scheduler
#define dead_band 40 //400 nanoseconds no of cycles * (1/100MHz)

// Phase angle for 500us blanking at 50Hz (approx 0.157 rad)
#define BLANKING_ANGLE_RAD 0.157f 
#define PI_VAL 3.14159265f
#define TWO_PI_VAL 6.2831853f

// Initialize the PLL object
SPLL_1PH_SOGI spll1;

volatile uint16_t currentDuty = 322; //volatile is used since it is modified in interrupt
volatile bool pwm_flag = false;

//for tripping logic

//rms calculation new
// 2048 counts = 325V. Scale = 325/2048 = 0.15869

volatile uint32_t sum_squares_ac =0;
volatile float final_rms = 0.0f;
volatile float final_dc_voltage = 0.0f;

//ADC timer_isr variables
volatile uint32_t temp_sum_squares = 0;
volatile uint16_t sample_counter = 0;
volatile uint16_t flatline_counter = 0;
volatile bool data_ready_flag = false;


const float ADC_SCALE_AC = 339.0f / 2048.0f;
const float ADC_SCALE_DC = 497.0f / 4096.0f;
const float ADC_SCALE_L_I = 9.972 / 2048.0f;

SPLL_1PH_SOGI spll1;
volatile float ac_vol_normalized = 0.0f;

// Control Loop Variables
volatile float v_dc_meas = 0.0f;
volatile float i_ac_meas = 0.0f;

volatile float v_dc_ref = 380.0f; // Target DC Bus Voltage
volatile float i_ref_amplitude = 0.0f; // Output of Voltage Loop
volatile float i_ref_inst = 0.0f;      // Instantaneous current reference
volatile float currentDutyFloat = 0.0f;

void initEPWM_HFL(void);
void initEPWM_LFL(void);
void initADC(void);
void initSCIA(void);

void sendSCIText(char *msg);


__interrupt void fastCurrentLoop_ISR(void);
__interrupt void slowVoltageLoop_ISR(void);


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
    Interrupt_register(INT_ADCA1, &fastCurrentLoop_ISR); // 50kHz
    Interrupt_register(INT_ADCA2, &slowVoltageLoop_ISR); // 10kHz


    // PinMux and Peripheral Initialization
    Board_init();
    initEPWM_HFL();
    initEPWM_LFL();
    initADC();
    initSCIA();

    SPLL_1PH_SOGI_reset(&spll1);
    // Config PLL for 10kHz ISR rate
    SPLL_1PH_SOGI_config(&spll1, GRID_FREQ, FAST_ISR_FREQ, 166.9743f, -166.2124f);
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
         // Background tasks (SCI/UART printing, state machine logic, fault checking)
        // Do NOT put control logic in the while loop.
        if(data_ready_flag)
        {
            float mean_square = (float)sum_squares_ac *0.001f;
            float rms_counts = sqrtf(mean_square);
            final_rms = rms_counts * ADC_SCALE_AC;

            final_dc_voltage = v_dc_meas;

            if(final_rms >= 50.0f && final_dc_voltage < 300.0f)
            {
                pwm_flag = true;
             

            }
            else if (final_rms < 40.0f)
            {
                pwm_flag = false;
                
               
            }
            else if (final_dc_voltage >= 350.0f) 
            {
                pwm_flag = false;
                
            }
            data_ready_flag = false;
            sprintf(buffer2, " DC_voltage : %d | Sin RMS: %d | duty : %u \r\n\r\n  ", (int)final_dc_voltage, (int)final_rms , currentDuty);
            sendSCIText(buffer2);
           
        }
       
    }
}

// SLOW LOOP: 10kHz (Triggered by EPWM6 SOCB prescaled by 5)
__interrupt void slowVoltageLoop_ISR(void)
{

    // 1. Read ADC for Voltage
    uint16_t raw_v_dc = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER1);
    

    v_dc_meas = (float)raw_v_dc * ADC_SCALE_DC;
    
    
    // 3. Voltage Loop PI Controller (runs at 10kHz)
    // NOTE: Insert your actual Voltage PI controller math here. 
    // Error = v_dc_ref - v_dc_meas;
    // i_ref_amplitude = PI_Output(Error); 
       
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER2);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP10);
}

// FAST LOOP: 50kHz (Triggered by EPWM6 SOCA prescaled by 1)

__interrupt void fastCurrentLoop_ISR(void)
{
    // 1. Read ADC for Current
        int16_t adj;
    uint16_t raw_i_ac = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER0);
    uint16_t raw_v_ac = ADC_readResult(ADCARESULT_BASE, ADC_SOC_NUMBER2);

        float ac_vol_normalized = ((float)raw_v_ac - 2048.0f) / 2048.0f;

    // 2. Run PLL
    SPLL_1PH_SOGI_run(&spll1, ac_vol_normalized);  // Normalize AC voltage for PLL (-1.0 to 1.0)
    // 3. Extract Phase Information
    // spll1.sine   -> Normalized sine wave in phase with Grid
    // spll1.theta  -> The phase angle (0 to 2*PI)

    i_ac_meas = ((float)raw_i_ac - 2048.0f) * ADC_SCALE_L_I;

    // 2. Generate instantaneous current reference
    // sine from SOGI is in phase with grid voltage
    i_ref_inst = i_ref_amplitude * spll1.sine; 



    // 3. Current Loop PI Controller (runs at 50kHz)
    // NOTE: Insert your actual Current PI controller math here.
    // Error = i_ref_inst - i_ac_meas;
    // DutyCycle = PI_Output(Error);
    
    // Dummy duty calculation for structure demonstration
    // currentDutyFloat = 0.5f; // Replace with PI output (0.0 to 1.0)
    // currentDuty = (uint16_t)(currentDutyFloat * PWM_PERIOD);

    // 4. Zero-Crossing Blanking Logic using Phase Angle (Theta)
    float theta = spll1.theta; // 0 to 2*PI


    bool in_blanking_window = (theta < BLANKING_ANGLE_RAD) || 
                              (theta > (PI_VAL - BLANKING_ANGLE_RAD) && theta < (PI_VAL + BLANKING_ANGLE_RAD)) ||
                              (theta > (TWO_PI_VAL - BLANKING_ANGLE_RAD));

    if (pwm_flag && !in_blanking_window) 
    {
        // Turn FETs ON
        EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);
        EPWM_clearTripZoneFlag(EPWM5_BASE, EPWM_TZ_FLAG_OST);

        // Grid Polarity based switching (Totem Pole Logic)
        if (spll1.sine >= 0) {
            // Positive Half Cycle
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, currentDuty);
            
            // LFL logic: Phase A tied low, Phase B tied high (depends on your hardware schematic)
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_HIGH); 
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_B, EPWM_AQ_SW_OUTPUT_LOW);
        } else {
            // Negative Half Cycle
            EPWM_setCounterCompareValue(EPWM6_BASE, EPWM_COUNTER_COMPARE_A, PWM_PERIOD - currentDuty);
            
            // LFL logic reversed
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_A, EPWM_AQ_SW_OUTPUT_LOW);
            EPWM_setActionQualifierContSWForceAction(EPWM5_BASE, EPWM_AQ_OUTPUT_B, EPWM_AQ_SW_OUTPUT_HIGH);
        }
    } 
    else 
    {
        // BLANKING WINDOW or FAULT: Force all OFF via Trip Zone
        EPWM_forceTripZoneEvent(EPWM6_BASE, EPWM_TZ_FORCE_EVENT_OST);
        EPWM_forceTripZoneEvent(EPWM5_BASE, EPWM_TZ_FORCE_EVENT_OST);
    }

    if(raw_v_ac <= 150)
    {
        flatline_counter++;
    }

    adj = (int16_t)raw_v_ac - ADC_OFFSET;
    temp_sum_squares += (uint32_t)((int32_t)adj * (int32_t)adj);
    sample_counter ++;
    if(sample_counter >= 1000)
    {
        if (flatline_counter < 480 )
        {
            sum_squares_ac = temp_sum_squares;
           
        }
        else
        {
            sum_squares_ac = 0;
        }
        sample_counter = 0;
        temp_sum_squares = 0;
        
        flatline_counter = 0;
        data_ready_flag = true;
    }



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

    // SOC0: AC Current (Fast Loop, Triggered by EPWM6 SOCA)
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER0, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN4, 15);
    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER0);
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);

//slow loop 
// Remove ADC_TRIGGER_SW_ONLY and replace with EPWM6 SOCA
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER1, ADC_TRIGGER_EPWM6_SOCB, ADC_CH_ADCIN6, 15);
    ADC_setupSOC(ADCA_BASE, ADC_SOC_NUMBER2, ADC_TRIGGER_EPWM6_SOCA, ADC_CH_ADCIN5, 15); //AC_voltage
        // SOC1 & SOC2: DC Voltage & AC Voltage (Slow Loop, Triggered by EPWM6 SOCB)
    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER2, ADC_SOC_NUMBER1);// Trigger on last conversion (SOC2)
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER2);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER2);
       // Generate INT2 at the end of SOC2 (Wait for both voltages to finish)
    // Prime the ADC so the first ISR pass has valid data to read
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER0);
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER1);
    ADC_forceSOC(ADCA_BASE, ADC_SOC_NUMBER2);

    Interrupt_enable(INT_ADCA1);
    Interrupt_enable(INT_ADCA2);
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
    EPWM_setADCTriggerSource(EPWM6_BASE, EPWM_SOC_A, EPWM_SOC_TBCTR_ZERO);
    EPWM_setADCTriggerSource(EPWM6_BASE, EPWM_SOC_B, EPWM_SOC_TBCTR_ZERO);
    
    // SOCA triggers every 1 count (50kHz -> Fast Loop)
    EPWM_setADCTriggerEventPrescale(EPWM6_BASE, EPWM_SOC_A, 1);
    // SOCB triggers every 5 counts (50kHz / 5 = 10kHz -> Slow Loop)
    EPWM_setADCTriggerEventPrescale(EPWM6_BASE, EPWM_SOC_B, 5); 
     // Trigger every 6th PWM pulse (10kHz control loop from 60kHz PWM)
    EPWM_enableADCTrigger(EPWM6_BASE, EPWM_SOC_A);
    EPWM_enableADCTrigger(EPWM6_BASE, EPWM_SOC_B);

    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZA, EPWM_TZ_ACTION_LOW);
    EPWM_setTripZoneAction(EPWM6_BASE, EPWM_TZ_ACTION_EVENT_TZB, EPWM_TZ_ACTION_LOW);
   // EPWM_clearTripZoneFlag(EPWM6_BASE, EPWM_TZ_FLAG_OST);

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

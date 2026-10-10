#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "driver/pulse_cnt.h"

#define PIN_A         4      /* sorties MCPWM (codeur) -> PA15 */
#define PIN_B         5      /* -> PA1 */
#define PIN_PWM_IN    14     /* PWM moteur venant de PA9 */

#define RES_HZ        10000000   /* resolution du timer codeur : 10 MHz */
#define TICKS_MIN     40         /* 250 kHz max par voie */
#define TICKS_MAX     60000      /* compteur 16 bits : en dessous de ~167 Hz le codeur est arrete */

/* Modele du moteur : premier ordre, omega' = (K * duty - omega) / TAU */
#define MOTOR_K_RPM   3000.0f    /* vitesse a 100 % de rapport cyclique */
#define MOTOR_TAU_S   0.100f     /* constante de temps mecanique */
#define MODEL_DT_S    0.001f     /* pas du modele : 1 ms */
#define ENC_LINES     500        /* traits par tour, comme ENC_LINES cote STM32 */

/* ---- Codeur simule (MCPWM, groupe 0) ---------------------------------- */

static mcpwm_timer_handle_t s_enc_timer;
static mcpwm_cmpr_handle_t  s_cA, s_cB1, s_cB2;
static bool                 s_enc_running;
static uint32_t             s_ticks;          /* periode courante en ticks */

static mcpwm_cmpr_handle_t new_cmp(mcpwm_oper_handle_t op, uint32_t val)
{
    mcpwm_cmpr_handle_t c;
    mcpwm_comparator_config_t cfg = { .flags.update_cmp_on_tez = true };
    ESP_ERROR_CHECK(mcpwm_new_comparator(op, &cfg, &c));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(c, val));
    return c;
}

static void start_quadrature(void)
{
    mcpwm_timer_config_t tcfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = RES_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = 2000,                       /* 5 kHz, remplace par le modele */
        .flags.update_period_on_empty = true,       /* periode et comparateurs changent ensemble */
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&tcfg, &s_enc_timer));
    s_ticks = tcfg.period_ticks;

    mcpwm_oper_handle_t opA, opB;
    mcpwm_operator_config_t ocfg = { .group_id = 0 };
    ESP_ERROR_CHECK(mcpwm_new_operator(&ocfg, &opA));
    ESP_ERROR_CHECK(mcpwm_new_operator(&ocfg, &opB));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(opA, s_enc_timer));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(opB, s_enc_timer));

    s_cA  = new_cmp(opA, s_ticks / 2);
    s_cB1 = new_cmp(opB, s_ticks / 4);
    s_cB2 = new_cmp(opB, 3 * s_ticks / 4);

    mcpwm_gen_handle_t genA, genB;
    mcpwm_generator_config_t gcA = { .gen_gpio_num = PIN_A };
    mcpwm_generator_config_t gcB = { .gen_gpio_num = PIN_B };
    ESP_ERROR_CHECK(mcpwm_new_generator(opA, &gcA, &genA));
    ESP_ERROR_CHECK(mcpwm_new_generator(opB, &gcB, &genB));

    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(genA,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                     MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genA,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cA, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genB,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cB1, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genB,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cB2, MCPWM_GEN_ACTION_LOW)));

    /* Le timer est active mais pas demarre : le moteur est a l'arret. */
    ESP_ERROR_CHECK(mcpwm_timer_enable(s_enc_timer));
}

/* Regle la frequence de chaque voie. Sous la frequence minimale (ou a 0),
   le codeur est arrete. Rien n'est ecrit si la periode n'a pas change.

   Tramage : la periode est un nombre entier de ticks, donc la frequence ne
   peut prendre que les valeurs RES/n (a 12,5 kHz : pas de 0,125 %). Pour que la
   frequence MOYENNE soit celle demandee, on alterne a chaque appel (1 ms) entre
   n et n+1 ticks, dans la proportion qui donne la bonne moyenne (accumulateur
   d'erreur, comme un modulateur sigma-delta du premier ordre). L'ecart entre
   n et n+1 est de 1 tick (100 ns) : les comparateurs bougent d'au plus 1 tick,
   meme si un front tombe entre deux ecritures. */
static void set_encoder_freq(float f_hz)
{
    static float s_acc;                       /* erreur de frequence accumulee */

    if (f_hz < (float)RES_HZ / TICKS_MAX) {
        if (s_enc_running) {
            ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_enc_timer, MCPWM_TIMER_STOP_EMPTY));
            s_enc_running = false;
        }
        return;
    }
    const uint32_t n = (uint32_t)((float)RES_HZ / f_hz);   /* periode entiere juste au-dessus de f */
    uint32_t ticks;
    if (n < TICKS_MIN) {
        ticks = TICKS_MIN;
    } else {
        const float f_hi = (float)RES_HZ / (float)n;         /* frequence avec n ticks   */
        const float f_lo = (float)RES_HZ / (float)(n + 1U);  /* frequence avec n+1 ticks */
        s_acc += (f_hz - f_lo) / (f_hi - f_lo);              /* part du temps passee a n */
        if (s_acc >= 1.0f) { s_acc -= 1.0f; ticks = n; }
        else               { ticks = n + 1U; }
    }
    if (ticks != s_ticks) {
        ESP_ERROR_CHECK(mcpwm_timer_set_period(s_enc_timer, ticks));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_cA,  ticks / 2));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_cB1, ticks / 4));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_cB2, 3 * ticks / 4));
        s_ticks = ticks;
    }
    if (!s_enc_running) {
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_enc_timer, MCPWM_TIMER_START_NO_STOP));
        s_enc_running = true;
    }
}

/* ---- Mesure du rapport cyclique de PA9 (MCPWM capture, groupe 1) ------ */

static portMUX_TYPE s_cap_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_pos;       /* date du dernier front montant */
static uint32_t s_last_high;      /* duree haute du dernier cycle  */
static bool     s_have_pos, s_have_high;
static uint32_t s_sum_high, s_sum_period, s_cycles;   /* cumuls depuis la derniere lecture */

/* A chaque front montant on cumule (duree haute du cycle precedent, periode),
   ce qui garde les deux sommes appariees : le rapport ne vacille pas d'un
   cycle a l'autre. */
static bool IRAM_ATTR on_capture(mcpwm_cap_channel_handle_t chan,
                                 const mcpwm_capture_event_data_t *ev, void *ctx)
{
    (void)chan; (void)ctx;
    portENTER_CRITICAL_ISR(&s_cap_lock);
    if (ev->cap_edge == MCPWM_CAP_EDGE_POS) {
        if (s_have_pos && s_have_high) {
            s_sum_high   += s_last_high;
            s_sum_period += ev->cap_value - s_last_pos;
            s_cycles++;
        }
        s_last_pos = ev->cap_value;
        s_have_pos = true;
        s_have_high = false;
    } else if (s_have_pos) {
        s_last_high = ev->cap_value - s_last_pos;
        s_have_high = true;
    }
    portEXIT_CRITICAL_ISR(&s_cap_lock);
    return false;
}

static void start_duty_capture(void)
{
    mcpwm_cap_timer_handle_t ctimer;
    mcpwm_capture_timer_config_t tcfg = {
        .group_id = 1,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&tcfg, &ctimer));

    mcpwm_cap_channel_handle_t chan;
    mcpwm_capture_channel_config_t ccfg = {
        .gpio_num = PIN_PWM_IN,
        .prescale = 1,
        .flags.pos_edge = true,
        .flags.neg_edge = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(ctimer, &ccfg, &chan));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_PWM_IN, GPIO_PULLDOWN_ONLY));   /* PA9 debranche = 0 % */

    mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_capture };
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(chan, &cbs, NULL));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(chan));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(ctimer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(ctimer));
}

/* Rapport cyclique moyen depuis la derniere lecture, 0..1.
   Sans front (PWM fige a 0 ou a 1) on lit le niveau de la broche. */
static float read_duty(void)
{
    uint32_t sh, sp, n;
    portENTER_CRITICAL(&s_cap_lock);
    sh = s_sum_high; sp = s_sum_period; n = s_cycles;
    s_sum_high = 0; s_sum_period = 0; s_cycles = 0;
    portEXIT_CRITICAL(&s_cap_lock);

    if (n == 0 || sp == 0) {
        return gpio_get_level(PIN_PWM_IN) ? 1.0f : 0.0f;
    }
    return (float)sh / (float)sp;
}

/* ---- Modele du moteur, 1 kHz ------------------------------------------ */

static volatile float g_duty;
static volatile float g_rpm;
static volatile float g_fa_hz;

static void motor_task(void *arg)
{
    (void)arg;
    float rpm = 0.0f;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1));

        const float duty = read_duty();
        rpm += (MODEL_DT_S / MOTOR_TAU_S) * (MOTOR_K_RPM * duty - rpm);

        const float fa = rpm / 60.0f * (float)ENC_LINES;   /* traits/s = frequence de A */
        set_encoder_freq(fa);

        g_duty = duty; g_rpm = rpm; g_fa_hz = fa;
    }
}

/* ---- Verification locale : decodeur x4 et compteurs de fronts sur A/B ---- */

static pcnt_unit_handle_t start_decoder(int pin_a, int pin_b)
{
    pcnt_unit_handle_t unit;
    pcnt_unit_config_t ucfg = { .high_limit = 30000, .low_limit = -30000 };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &fcfg));

    pcnt_chan_config_t c1 = { .edge_gpio_num = pin_a, .level_gpio_num = pin_b };
    pcnt_chan_config_t c2 = { .edge_gpio_num = pin_b, .level_gpio_num = pin_a };
    pcnt_channel_handle_t ch1, ch2;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &c1, &ch1));
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &c2, &ch2));

    /* Decodage x4 : chaque canal compte les deux fronts de son signal,
       l'autre signal inverse le sens quand il est bas. */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch1,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch1,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch2,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch2,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));
    return unit;
}

/* Compte les deux fronts d'une seule broche : sert a verifier chaque voie. */
static pcnt_unit_handle_t start_edge_counter(int pin)
{
    pcnt_unit_handle_t unit;
    pcnt_unit_config_t ucfg = { .high_limit = 30000, .low_limit = -30000 };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &fcfg));

    pcnt_chan_config_t c = { .edge_gpio_num = pin, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &c, &ch));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));
    return unit;
}

void app_main(void)
{
    start_quadrature();
    start_duty_capture();
    /* Le PCNT lit directement les broches de sortie A/B (une sortie peut aussi
       etre lue en entree) : pas de fil de rebouclage. */
    pcnt_unit_handle_t dec  = start_decoder(PIN_A, PIN_B);
    pcnt_unit_handle_t cntA = start_edge_counter(PIN_A);
    pcnt_unit_handle_t cntB = start_edge_counter(PIN_B);
    xTaskCreate(motor_task, "motor", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
    printf("moteur : K=%.0f rpm, tau=%.0f ms, %d traits ; PWM sur GPIO%d, codeur A=GPIO%d B=GPIO%d\n",
           (double)MOTOR_K_RPM, (double)(MOTOR_TAU_S * 1000.0f), ENC_LINES,
           PIN_PWM_IN, PIN_A, PIN_B);

    /* Fenetre de 250 ms : a 3000 rpm il y a 100 000 comptes/s, la limite
       du compteur (30 000) serait depassee en 1 s. */
    for (;;) {
        int x4 = 0, ea = 0, eb = 0;
        pcnt_unit_clear_count(dec);
        pcnt_unit_clear_count(cntA);
        pcnt_unit_clear_count(cntB);
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(250));
        pcnt_unit_get_count(dec, &x4);
        pcnt_unit_get_count(cntA, &ea);
        pcnt_unit_get_count(cntB, &eb);
        const double dt = (double)(esp_timer_get_time() - t0) / 1e6;

        const float duty = g_duty, rpm = g_rpm, fa = g_fa_hz;
        printf("duty=%5.1f %%  modele=%7.1f rpm  fA=%8.1f Hz | x4=%9.0f/s (attendu %9.0f)  fronts A=%8.0f/s B=%8.0f/s (attendu %8.0f)\n",
               (double)(duty * 100.0f), (double)rpm, (double)fa,
               (double)x4 / dt, (double)(4.0f * fa),
               (double)ea / dt, (double)eb / dt, (double)(2.0f * fa));
    }
}

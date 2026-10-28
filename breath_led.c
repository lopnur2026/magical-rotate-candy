/*
 * ============================================================================
 * 文件: breath_led.c
 * 硬件: STM32F103C8T6 (C8T6 最小系统板)
 * 引脚: PB0  —— 板载 LED 或外接 LED（串联 220~470Ω 限流电阻）
 * 功能: 无开关呼吸灯，上电自动循环
 *       - 渐亮（暗 -> 最亮）保持 1000ms
 *       - 渐灭（最亮 -> 暗）保持 1000ms
 *       - 一个完整呼吸周期 = 2000ms
 *
 * 原理大白话：
 *   PB0 这个引脚除了能当普通 IO，还能被定时器 TIM3 的第 3 通道接管，
 *   输出一种叫 PWM（脉冲宽度调制）的方波。占空比 = 高电平时间 / 周期，
 *   占空比越大，LED 平均电压越高，看起来越亮。
 *   我们让占空比一点点从 0 爬到 100%，再一点点退回来，就形成了呼吸效果。
 *
 * 接线说明：
 *   LED 正极 --[限流电阻]--> PB0
 *   LED 负极 -------------> GND
 *   （若你的 LED 是反过来接的共阳接法，把下面的 TIM_OCPolarity_High
 *      改成 TIM_OCPolarity_Low 即可）
 *
 * 库依赖: STM32 标准外设库 (StdPeriph_DRIVERS)，需在工程里勾选:
 *         stm32f10x_gpio.c / stm32f10x_rcc.c / stm32f10x_tim.c / misc.c
 * ============================================================================
 */

#include "stm32f10x.h"

/* -------------------- 呼吸参数（想改手感只动这里） -------------------- */
#define PWM_LEVEL       1000    /* 亮度等级数：0~999 共 1000 级           */
#define RISE_MS         1000    /* 渐亮时长：1000ms                       */
#define FALL_MS         1000   /* 渐灭时长：1000ms                       */
#define STEP_MS         (RISE_MS / PWM_LEVEL)  /* 每步延时 = 1ms        */

/* SysTick 毫秒时基用到的计数重装载值：72MHz / 1000 = 72000 */
#define SYSTICK_LOAD_1MS    (SystemCoreClock / 1000)

/**
 * @brief  毫秒级延时（基于 SysTick 查询，不占用中断）
 * @param  ms: 要延时的毫秒数
 */
static void Delay_ms(uint32_t ms)
{
    /* 配置 SysTick：时钟源 = HCLK(72MHz)，每 1ms 产生一次 COUNTFLAG */
    SysTick->LOAD = SYSTICK_LOAD_1MS - 1;       /* 重装载值                */
    SysTick->VAL  = 0;                          /* 清空当前计数值          */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk /* 用内核时钟 72MHz        */
                  | SysTick_CTRL_ENABLE_Msk;    /* 启动 SysTick            */

    while (ms--)
    {
        /* 等待 COUNTFLAG 置 1，表示这 1ms 走完了 */
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0)
        {
            /* 空等 */
        }
    }

    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk; /* 关掉 SysTick，省点电    */
}

/**
 * @brief  初始化 PB0 为 TIM3_CH3 的 PWM 输出
 *
 * 引脚复用关系（数据手册查得）：
 *   PB0  --AF-->  TIM3_CH3
 *
 * PWM 参数推导：
 *   定时器输入时钟 = APB1 倍频后 = 72MHz
 *   PWM 频率 = 72MHz / (PSC+1) / (ARR+1)
 *   取 ARR = 999（即 PWM_LEVEL-1，亮度分辨率 1000 级）
 *   则 PSC+1 = 72MHz / 1000 / 1000 = 72  -> PSC = 71
 *   最终 PWM 频率 = 1kHz，远超人眼闪烁感知阈值，看起来就是平滑调光
 */
static void BreathLED_Init(void)
{
    GPIO_InitTypeDef        GPIO_InitStruct;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStruct;
    TIM_OCInitTypeDef       TIM_OCInitStruct;

    /* 1) 开时钟：GPIOB 在 APB2，TIM3 在 APB1 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    /* 2) PB0 配成“复用推挽输出”——引脚控制权交给定时器，不能用普通推挽 */
    GPIO_InitStruct.GPIO_Pin   = GPIO_Pin_0;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_AF_PP;   /* 复用推挽             */
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* 3) 配 TIM3 时基 */
    TIM_TimeBaseStruct.TIM_Period        = PWM_LEVEL - 1;       /* ARR = 999  */
    TIM_TimeBaseStruct.TIM_Prescaler     = 72 - 1;             /* PSC = 71   */
    TIM_TimeBaseStruct.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStruct.TIM_CounterMode   = TIM_CounterMode_Up; /* 向上计数   */
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStruct);

    /* 4) 配 TIM3 通道 3（对应 PB0）为 PWM1 模式 */
    TIM_OCInitStruct.TIM_OCMode      = TIM_OCMode_PWM1;       /* PWM1：CNT<CCR 时输出有效 */
    TIM_OCInitStruct.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStruct.TIM_Pulse       = 0;                     /* 初值占空比 0%（灭） */
    TIM_OCInitStruct.TIM_OCPolarity  = TIM_OCPolarity_High;   /* 有效电平=高     */
    TIM_OC3Init(TIM3, &TIM_OCInitStruct);

    /* 允许 CCR3 寄存器预装载：写新值不会立刻打断当前 PWM 周期，波形更干净 */
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);

    /* 5) 启动 TIM3 */
    TIM_Cmd(TIM3, ENABLE);
}

/**
 * @brief  主函数：上电即开始呼吸，无需任何按键
 */
int main(void)
{
    uint16_t duty;   /* 当前占空比 0~999 */

    BreathLED_Init();

    while (1)
    {
        /* ---- 阶段一：渐亮，从 0% 爬到 100%，共 1000 步 × 1ms = 1000ms ---- */
        for (duty = 0; duty < PWM_LEVEL; duty++)
        {
            TIM_SetCompare3(TIM3, duty);   /* 改 CCR3 = 改占空比           */
            Delay_ms(STEP_MS);             /* 每步停 1ms，共 1000ms         */
        }

        /* ---- 阶段二：渐灭，从 100% 退到 0%，共 1000 步 × 1ms = 1000ms ----
         * 用 duty 从 PWM_LEVEL 递减到 1，CCR3 取 duty-1，正好覆盖 999->0，
         * 避免 uint16_t 减到 0 后下溢成 65535 的死循环。                       */
        for (duty = PWM_LEVEL; duty > 0; duty--)
        {
            TIM_SetCompare3(TIM3, duty - 1);
            Delay_ms(STEP_MS);
        }
    }
}

/* ============================================================
 * 备忘：
 *   1. 如果下载后 LED 不亮，先万用表量 PB0 是否有 1.6V 左右平均电压；
 *      没有就查 GPIOB/TIM3 时钟是否打开、PB0 是否配成 AF_PP。
 *   2. 如果呼吸方向反了（上电先灭再亮是对的；如果先亮后灭就是极性反了），
 *      把 TIM_OCPolarity_High 改成 TIM_OCPolarity_Low。
 *   3. 想改呼吸快慢：只改 RISE_MS / FALL_MS 即可，STEP_MS 会自动重算。
 *   4. 想换引脚：PA6/PB4 是 TIM3_CH1，PC8 是 TIM3_CH3 复用脚，
 *      换引脚时别忘了改对应的 GPIO 初始化和 TIM_SetCompareX。
 * ============================================================
 */

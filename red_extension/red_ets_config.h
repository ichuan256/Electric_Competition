#ifndef RED_ETS_CONFIG_H
#define RED_ETS_CONFIG_H

/*
 * Red equivalent-time sequential sampling extension configuration.
 *
 * The core never includes MSPM0 DriverLib headers and never names a concrete
 * timer, ADC, DMA channel, GPIO or event route.  The resource macros below are
 * deliberately opaque: an integrator may redefine them in the compiler command
 * line or in a project-wide header without modifying any extension source.
 */

#ifndef RED_ENABLE_ETS
#define RED_ENABLE_ETS                         1
#endif

#ifndef RED_ETS_TIMER_RESOURCE
#define RED_ETS_TIMER_RESOURCE                 0u
#endif
#ifndef RED_ETS_ADC_RESOURCE
#define RED_ETS_ADC_RESOURCE                   0u
#endif
#ifndef RED_ETS_DMA_RESOURCE
#define RED_ETS_DMA_RESOURCE                   0u
#endif
#ifndef RED_ETS_COMPARATOR_RESOURCE
#define RED_ETS_COMPARATOR_RESOURCE            0u
#endif
#ifndef RED_ETS_EVENT_ROUTE_RESOURCE
#define RED_ETS_EVENT_ROUTE_RESOURCE           0u
#endif

#ifndef RED_ETS_TIMER_CLOCK_HZ_DEFAULT
#define RED_ETS_TIMER_CLOCK_HZ_DEFAULT         80000000u
#endif
#ifndef RED_ETS_ADC_MAX_RATE_HZ_DEFAULT
#define RED_ETS_ADC_MAX_RATE_HZ_DEFAULT        4000000u
#endif
#ifndef RED_ETS_MIN_FUNDAMENTAL_HZ_DEFAULT
#define RED_ETS_MIN_FUNDAMENTAL_HZ_DEFAULT     100000u
#endif
#ifndef RED_ETS_MAX_FUNDAMENTAL_HZ_DEFAULT
#define RED_ETS_MAX_FUNDAMENTAL_HZ_DEFAULT     1000000u
#endif
#ifndef RED_ETS_MAX_PHASE_BINS
#define RED_ETS_MAX_PHASE_BINS                 512u
#endif
#ifndef RED_ETS_MAX_SPECTRUM_BINS
#define RED_ETS_MAX_SPECTRUM_BINS              32u
#endif
#ifndef RED_ETS_PERIOD_SAMPLES_DEFAULT
#define RED_ETS_PERIOD_SAMPLES_DEFAULT         64u
#endif
#ifndef RED_ETS_PERIOD_DISCARD_DEFAULT
#define RED_ETS_PERIOD_DISCARD_DEFAULT         8u
#endif
#ifndef RED_ETS_AVERAGES_DEFAULT
#define RED_ETS_AVERAGES_DEFAULT               8u
#endif
#ifndef RED_ETS_HARMONIC_LIMIT_DEFAULT
#define RED_ETS_HARMONIC_LIMIT_DEFAULT         5u
#endif
#ifndef RED_ETS_FRONTEND_SETTLE_MS_DEFAULT
#define RED_ETS_FRONTEND_SETTLE_MS_DEFAULT     300u
#endif
#ifndef RED_ETS_DEADLINE_MS_DEFAULT
#define RED_ETS_DEADLINE_MS_DEFAULT            10000u
#endif
#ifndef RED_ETS_TRIGGER_TIMEOUT_MS_DEFAULT
#define RED_ETS_TRIGGER_TIMEOUT_MS_DEFAULT     100u
#endif
#ifndef RED_ETS_PHASE_TIMEOUT_MS_DEFAULT
#define RED_ETS_PHASE_TIMEOUT_MS_DEFAULT       100u
#endif
#ifndef RED_ETS_JITTER_LIMIT_PPM_DEFAULT
#define RED_ETS_JITTER_LIMIT_PPM_DEFAULT       2000u
#endif
#ifndef RED_ETS_ADC_CLIP_LOW_CODE_DEFAULT
#define RED_ETS_ADC_CLIP_LOW_CODE_DEFAULT      248u
#endif
#ifndef RED_ETS_ADC_CLIP_HIGH_CODE_DEFAULT
#define RED_ETS_ADC_CLIP_HIGH_CODE_DEFAULT     3847u
#endif

/* Set to zero when the existing Red project supplies its own FFT backend. */
#ifndef RED_ETS_ENABLE_REFERENCE_DFT
#define RED_ETS_ENABLE_REFERENCE_DFT           1
#endif

#ifndef RED_ETS_PROTOCOL_VERSION
#define RED_ETS_PROTOCOL_VERSION               0x01u
#endif
#ifndef RED_ETS_NODE_BLUE
#define RED_ETS_NODE_BLUE                      0x02u
#endif
#ifndef RED_ETS_NODE_RED
#define RED_ETS_NODE_RED                       0x03u
#endif
#ifndef RED_ETS_PROTOCOL_MAX_PAYLOAD
#define RED_ETS_PROTOCOL_MAX_PAYLOAD           128u
#endif

#define RED_ETS_HELLO_CAPABILITY_BIT           (1u << 6)

#endif

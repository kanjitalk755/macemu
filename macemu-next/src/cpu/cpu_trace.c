/**
 * CPU Execution Tracing Implementation
 */

#include "cpu_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CPUTraceState g_trace = {0};

void cpu_trace_init(void) {
	const char *trace_env = getenv("CPU_TRACE");
	if (!trace_env) {
		g_trace.enabled = false;
		g_trace.trace_memory = false;
		return;
	}

	/* Parse CPU_TRACE value */
	if (strchr(trace_env, '-')) {
		/* Range format: "start-end" */
		if (sscanf(trace_env, "%lu-%lu", &g_trace.start_count, &g_trace.end_count) == 2) {
			g_trace.enabled = true;
			/* Only print banner if CPU_TRACE_QUIET is not set */
			if (!getenv("CPU_TRACE_QUIET")) {
				fprintf(stderr, "[CPU trace enabled: instructions %lu-%lu]\n",
				        g_trace.start_count, g_trace.end_count);
			}
		}
	} else {
		/* Simple count: "N" means trace first N instructions */
		long count = strtol(trace_env, NULL, 10);
		if (count > 0) {
			g_trace.enabled = true;
			g_trace.start_count = 0;
			g_trace.end_count = count;
			/* Only print banner if CPU_TRACE_QUIET is not set */
			if (!getenv("CPU_TRACE_QUIET")) {
				fprintf(stderr, "[CPU trace enabled: will trace first %ld instructions]\n", count);
			}
		}
	}

	/* Check if memory tracing is enabled */
	g_trace.trace_memory = (getenv("CPU_TRACE_MEMORY") != NULL);

	g_trace.current_count = 0;
}

bool cpu_trace_should_log(void) {
	if (!g_trace.enabled) return false;
	return (g_trace.current_count >= g_trace.start_count &&
	        g_trace.current_count < g_trace.end_count);
}

void cpu_trace_increment(void) {
	g_trace.current_count++;
}

void cpu_trace_log_simple(uint32_t pc, uint16_t opcode,
                          uint32_t d0, uint32_t d1,
                          uint32_t a0, uint32_t a7,
                          uint16_t sr) {
	fprintf(stdout, "[%04lu] PC=%08X OP=%04X | D0=%08X D1=%08X A0=%08X A7=%08X SR=%04X\n",
	        g_trace.current_count, pc, opcode, d0, d1, a0, a7, sr);
}

void cpu_trace_log(uint32_t pc, uint16_t opcode,
                   uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                   uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7,
                   uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                   uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                   uint16_t sr) {
	fprintf(stdout, "[%04lu] PC=%08X OP=%04X | "
	        "D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X | "
	        "A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X | "
	        "SR=%04X\n",
	        g_trace.current_count, pc, opcode,
	        d0, d1, d2, d3, d4, d5, d6, d7,
	        a0, a1, a2, a3, a4, a5, a6, a7,
	        sr);
}

void cpu_trace_log_detailed(const char *cpu_name, uint32_t pc, uint16_t opcode,
                             uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                             uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7,
                             uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                             uint16_t sr) {
	/* Extract condition codes from SR */
	uint8_t ccr = sr & 0x1F;
	uint8_t x = (ccr >> 4) & 1;
	uint8_t n = (ccr >> 3) & 1;
	uint8_t z = (ccr >> 2) & 1;
	uint8_t v = (ccr >> 1) & 1;
	uint8_t c = (ccr >> 0) & 1;

	/* Format: [count] PC OP | Dregs | Aregs | SR [flags] */
	fprintf(stdout, "[%05lu] %08X %04X | "
	        "%08X %08X %08X %08X %08X %08X %08X %08X | "
	        "%08X %08X %08X %08X %08X %08X %08X %08X | "
	        "%04X %d%d%d%d%d\n",
	        g_trace.current_count, pc, opcode,
	        d0, d1, d2, d3, d4, d5, d6, d7,
	        a0, a1, a2, a3, a4, a5, a6, a7,
	        sr, x, n, z, v, c);
	fflush(stdout);
}

void cpu_trace_log_mem_read(uint32_t addr, uint32_t value, int size) {
	if (!g_trace.trace_memory) return;
	if (!cpu_trace_should_log()) return;

	/* Format: "  MEM[addr] = value (size)" on same line as instruction */
	const char *size_str = (size == 1) ? "B" : (size == 2) ? "W" : "L";
	fprintf(stdout, "  MEM[%08X]=%08X (%s)\n", addr, value, size_str);
	fflush(stdout);
}

bool cpu_trace_memory_enabled(void) {
	return g_trace.trace_memory && cpu_trace_should_log();
}

void cpu_trace_log_interrupt_trigger(int level) {
	if (!g_trace.enabled) return;  /* Zero cost if disabled */
	/* Format: [count] @@INTR_TRIG level */
	fprintf(stdout, "[%05lu] @@INTR_TRIG %d\n",
	        g_trace.current_count, level);
	fflush(stdout);
}

void cpu_trace_log_interrupt_taken(int level, uint32_t handler_addr) {
	if (!cpu_trace_should_log()) return;  /* Zero cost if disabled or outside range */
	/* Format: [count] @@INTR_TAKE level handler_addr */
	fprintf(stdout, "[%05lu] @@INTR_TAKE %d %08X\n",
	        g_trace.current_count, level, handler_addr);
	fflush(stdout);
}

void cpu_trace_log_emulop(uint16_t opcode) {
	if (!cpu_trace_should_log()) return;  /* Zero cost if disabled or outside range */
	/* Format: [count] @@EMULOP opcode */
	fprintf(stdout, "[%05lu] @@EMULOP %04X\n",
	        g_trace.current_count, opcode);
	fflush(stdout);
}

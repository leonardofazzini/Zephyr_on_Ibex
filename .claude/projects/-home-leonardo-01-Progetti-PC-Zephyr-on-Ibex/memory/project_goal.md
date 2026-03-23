---
name: Project goal - Zephyr on Ibex porting
description: Main project objective is porting Zephyr RTOS to the Ibex RISC-V core (Secure-Ibex variant)
type: project
---

The goal of this project is to port Zephyr RTOS onto the Ibex RISC-V core.

**Why:** The user wants to run Zephyr on their custom Secure-Ibex hardware platform.

**How to apply:** All work in this repo revolves around making Zephyr run on Ibex. The Ibex HW description lives in the submodule `hw/Secure-Ibex` (git@github.com:leonardofazzini/Secure-Ibex.git). Future steps will involve creating Zephyr board/SoC definitions, device tree, and drivers targeting this hardware.

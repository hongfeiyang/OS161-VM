# OS/161: Educational Operating System

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

OS/161 is an educational operating system designed for use in operating systems courses. It provides a simplified but realistic environment for implementing and understanding core operating system concepts.

## Overview

OS/161 is designed for implementing core operating system components:
- Process management
- Thread management
- Virtual memory
- File systems
- System calls
- Synchronization primitives

This educational OS runs on System/161, a machine simulator that emulates a simplified MIPS R3000 processor and provides various virtual devices.

## Custom Implementation Highlights

This fork features a custom implementation of several key operating system components:

### Virtual Memory System
- Two-level page table implementation for efficient memory management
- Dynamic page allocation on demand (demand paging)
- Memory permission management (read/write/execute)
- TLB management with proper invalidation

### Process Management
- Process forking with memory management
- Page table copying between parent and child processes
- Memory mapping for executable loading

### Implementation Details
- L1 and L2 page tables with hierarchical structure
- Page fault handling for on-demand page allocation
- VM fault handling with proper permission checking
- Memory regions with permission tracking
- Proper TLB shootdown mechanism

## System Architecture Diagrams

### Two-Level Page Table Structure
```mermaid
graph TD
    VA[Virtual Address] --> |"L1_INDEX(VA)"| L1PT[L1 Page Table]
    VA --> |"L2_INDEX(VA)"| L2IDX[L2 Index]
    VA --> |"OFFSET_BITS"| Offset[Page Offset]
    
    L1PT --> |Lookup| L2PT[L2 Page Table]
    L2IDX --> |Index into L2PT| L2PT
    L2PT --> |"PTE"| PA[Physical Address]
    Offset --> PA
    
    subgraph "Virtual Address Bits"
        VA
    end
    
    subgraph "Page Table Structure"
        L1PT
        L2PT
    end
    
    subgraph "L1 Page Table"
        L1E1[Entry 0]
        L1E2[Entry 1]
        L1E3[...]
        L1E4[Entry 2047]
    end
    
    subgraph "L2 Page Table"
        L2E1[Entry 0]
        L2E2[Entry 1]
        L2E3[...]
        L2E4[Entry 511]
    end
    
    L1PT --- L1E1
    L1PT --- L1E2
    L1PT --- L1E3
    L1PT --- L1E4
    
    L2PT --- L2E1
    L2PT --- L2E2
    L2PT --- L2E3
    L2PT --- L2E4
    
    L2E2 --> |Contains| PTE[Page Table Entry]
    PTE --> |Frame| PhysAddr[Physical Frame]
    PhysAddr --> |Plus Offset| FinalPA[Final Physical Address]
```

### VM Fault Handling Process
```mermaid
flowchart TD
    Start([VM Fault]) --> CheckType{Fault Type?}
    CheckType -->|Read/Write| LookupPT[Look up Page Table]
    CheckType -->|Read-Only| ReturnRO[Return EFAULT]
    
    LookupPT --> Found{PTE Found?}
    Found -->|Yes| LoadTLB[Load TLB Entry]
    Found -->|No| CheckRegion[Check Address Space Regions]
    
    CheckRegion --> InRegion{Valid Region?}
    InRegion -->|No| ReturnInvalid[Return EFAULT]
    InRegion -->|Yes| CheckPerms{Check Permissions}
    
    CheckPerms -->|Valid| AllocPage[Allocate Physical Page]
    CheckPerms -->|Invalid| ReturnPerm[Return EFAULT]
    
    AllocPage --> ZeroFill[Zero-fill Page]
    ZeroFill --> AddPTE[Add Page Table Entry]
    AddPTE --> LoadTLB
    
    LoadTLB --> ReturnSuccess[Return Success]
```

### Process Fork Memory Management
```mermaid
flowchart TD
    Start([Fork Process]) --> CopyAS[Copy Address Space]
    CopyAS --> CreateNewAS[Create New Page Table]
    CreateNewAS --> CopyRegions[Copy Memory Regions]
    
    CopyRegions --> IterateRegions[Iterate Through Regions]
    IterateRegions --> CopyOnWrite{Use Copy-on-Write?}
    
    CopyOnWrite -->|Yes| SharePages[Share Pages & Mark Read-Only]
    CopyOnWrite -->|No| CopyPages[Copy Pages to Child]
    
    SharePages --> IncRefCount[Increment Reference Count]
    IncRefCount --> MarkRO[Mark Pages Read-Only]
    MarkRO --> SetTLBDirty[Clear TLBLO_DIRTY Flag]
    
    CopyPages --> AllocNewPages[Allocate New Pages]
    AllocNewPages --> CopyContent[Copy Memory Content]
    CopyContent --> AddToChildPT[Add to Child Page Table]
    
    SetTLBDirty --> HandleVM{VM Fault Handler}
    AddToChildPT --> HandleVM
    
    HandleVM --> WriteAttempt{Write Attempt?}
    WriteAttempt -->|Yes, Shared Page| COWPage[Copy Page on Write]
    WriteAttempt -->|Regular Access| RegularAccess[Regular TLB Load]
    
    COWPage --> CheckRefCount[Check Reference Count]
    CheckRefCount --> CopyPageContent[Copy Page Content]
    CopyPageContent --> UpdatePTE[Update Page Table Entry]
    UpdatePTE --> ReturnToProcess[Return to Process]
    
    RegularAccess --> ReturnToProcess
```

## Project Structure

- **kern/** - Kernel source code
  - **arch/** - Architecture-specific code (MIPS)
  - **conf/** - Kernel configuration
  - **dev/** - Device drivers
  - **fs/** - File system implementations
  - **include/** - Kernel header files
  - **lib/** - Kernel utility functions
  - **main/** - Kernel initialization
  - **proc/** - Process management
  - **syscall/** - System call implementations
  - **test/** - Kernel test code
  - **thread/** - Thread management
  - **vm/** - Virtual memory subsystem
  - **vfs/** - Virtual file system

- **userland/** - User programs and libraries
  - **bin/** - Basic user programs
  - **include/** - User-level header files
  - **lib/** - User-level libraries
  - **sbin/** - System utilities
  - **testbin/** - Test programs

- **mk/** - Build system files
- **common/** - Common components
- **design/** - Design documentation
- **man/** - Manual pages
- **testscripts/** - Automated test scripts

## Getting Started

### Prerequisites

- GCC MIPS cross-compiler
- Python (2.7 or later)
- System/161 simulator

### Building OS/161

1. Configure the build environment:
   ```
   ./configure --ostree=$HOME/os161/root
   ```

2. Build the OS/161 kernel:
   ```
   cd kern/conf
   ./config DUMBVM
   cd ../compile/DUMBVM
   bmake depend
   bmake
   bmake install
   ```

3. Build userland programs:
   ```
   cd userland
   bmake
   bmake install
   ```

### Running OS/161

Run the OS/161 kernel with:
```
cd $HOME/os161/root
sys161 kernel
```

## Development Tasks

OS/161 is designed for implementing various OS components as part of coursework:

1. **Synchronization primitives** - Implementing locks, semaphores, and condition variables
2. **System calls** - Adding basic system calls like read, write, and exit
3. **Process management** - Implementing process creation, execution, and termination
4. **Virtual memory** - Adding virtual memory with paging and TLB management
5. **File systems** - Implementing a simple file system

## Testing

The project includes various test programs in `userland/testbin/` that can be used to validate your implementations.

Automated test scripts are available in the `testscripts/` directory.

## Resources

- OS/161 was originally developed at Harvard University by David A. Holland
- The current version is maintained for educational use in operating systems courses

## Contributors

OS/161 was developed by David A. Holland with contributions from:
- Amos Blackman
- Alexandra Fedorova
- Ada T. Lim
- Georgi Matev
- Jay Moorthi
- Geoffrey Werner-Allen

Custom virtual memory implementation contributions by:
- Hongfei Yang - Two-level page table, virtual memory subsystem, process forking with memory management

And others as listed in the CHANGES file.

## License

OS/161 is distributed under an MIT-like educational license. 
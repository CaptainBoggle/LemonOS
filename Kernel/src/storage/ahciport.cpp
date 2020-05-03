#include <ahci.h>

#include <physicalallocator.h>
#include <paging.h>
#include <logging.h>
#include <gpt.h>

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ 0x08

#define ATA_CMD_READ_DMA_EX     0x25
#define HBA_PxIS_TFES   (1 << 30)

namespace AHCI{
	Port::Port(int num, hba_port_t* portStructure){
		registers->cmd &= ~HBA_PxCMD_ST;
		registers->cmd &= ~HBA_PxCMD_FRE;

        registers = portStructure;

        //stopCMD(registers);

        uintptr_t phys;
        
		// Command list entry size = 32
		// Command list entry maxim count = 32
		// Command list maxim size = 32*32 = 1K per port
        phys = Memory::AllocatePhysicalMemoryBlock();
		registers->clb = (uint32_t)(phys & 0xFFFFFFFF);
		registers->clbu = (uint32_t)(phys >> 32);
	
		// FIS entry size = 256 bytes per port
        phys = Memory::AllocatePhysicalMemoryBlock();
		registers->fb = (uint32_t)(phys & 0xFFFFFFFF);
		registers->fbu = (uint32_t)(phys >> 32);
	
		// Command list size = 256*32 = 8K per port
		commandList = (hba_cmd_header_t*)Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(registers->clb, (uintptr_t)commandList, 1);
        registers->clbu = 0;
        memset(commandList, 0, PAGE_SIZE_4K);

		// FIS
		fis = Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(registers->fb, (uintptr_t)fis, 1);
        memset(fis, 0, PAGE_SIZE_4K);

        for(int i = 0; i < 8 /*Support for 8 command slots*/; i++){
            commandList[i].prdtl = 1;

            phys = Memory::AllocatePhysicalMemoryBlock();
            commandList[i].ctba = (uint32_t)(phys & 0xFFFFFFFF);
            commandList[i].ctbau = (uint32_t)(phys >> 32);

            commandTables[i] = (hba_cmd_tbl_t*)Memory::KernelAllocate4KPages(1);
            Memory::KernelMapVirtualMemory4K(phys,(uintptr_t)commandTables[i], 1);
            memset(commandTables[i],1,PAGE_SIZE_4K);
        }

        registers->is = 0;

		registers->cmd |= HBA_PxCMD_FRE;
		registers->cmd |= HBA_PxCMD_ST; 

        bufPhys = Memory::AllocatePhysicalMemoryBlock();
        bufVirt = Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(bufPhys, (uintptr_t)bufVirt, 1);

        switch(GPT::Parse(this)){
        case 0:
            Log::Error("[SATA] Disk has a corrupted or non-existant GPT. MBR disks are NOT supported.");
            break;
        case -1:
            Log::Error("[SATA] Disk Error while Parsing GPT for SATA Disk ");
            break;
        }

        InitializePartitions();
    }

    int Port::Read(uint64_t lba, uint32_t count, void* buffer){
        uint64_t blockCount = ((count / 512 * 512) < count) ? ((count / 512) + 1) : (count / 512);

        while(blockCount-- && count){
            uint64_t size;
            if(count < 512) size = count;
            else size = 512;

            if(!size) continue;

            if(InternalRead(lba, 1)){
                return 1; // Error Reading Sectors
            }

            memcpy(buffer, bufVirt, size);
            buffer += size;
            lba++;
        }

        return 0;
    }

    int Port::InternalRead(uint64_t lba, uint32_t count){
        registers->is = 0xffff; 
        int spin = 0;


        int slot = FindCmdSlot();
        if(slot == -1){
            Log::Warning("[SATA] Could not find command slot!");
            return 2;
        }

        hba_cmd_header_t* commandHeader = &commandList[slot];

        commandHeader->cfl = sizeof(fis_reg_h2d_t) / 4;
        
        commandHeader->w = 0;               // Read from device
        commandHeader->c = 1;               // Read from device
        commandHeader->p = 1;               // Read from device

        hba_cmd_tbl_t* commandTable = commandTables[slot];

        commandTable->prdt_entry[0].dba = bufPhys & 0xFFFFFFFF;
        commandTable->prdt_entry[0].dbau = (bufPhys >> 32) & 0xFFFFFFFF;
        commandTable->prdt_entry[0].dbc = 511; // 512 bytes per sector
        commandTable->prdt_entry[0].i = 0;

        fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)(commandTable->cfis); 
        memset(commandTable->cfis, 0, sizeof(fis_reg_h2d_t));

        cmdfis->fis_type = FIS_TYPE_REG_H2D;
        cmdfis->c = 1;  // Command
        cmdfis->pmport = 0; // Port multiplier
        cmdfis->command = ATA_CMD_READ_DMA_EX;
 
        cmdfis->lba0 = lba & 0xFF;
        cmdfis->lba1 = (lba >> 8) & 0xFF;
        cmdfis->lba2 = (lba >> 16) & 0xFF;
        cmdfis->device = 1 << 6;
 
        cmdfis->lba3 = (lba >> 24) & 0xFF;
        cmdfis->lba4 = (lba >> 32) & 0xFF;
        cmdfis->lba5 = (lba >> 40) & 0xFF;
 
        cmdfis->countl = count & 0xff;
        cmdfis->counth = count >> 8;

        while ((registers->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000) {
            spin++;
        }

        if(spin >= 1000000){
            Log::Warning("[SATA] Port Hung");
            return 3;
        }

        startCMD(registers);
        registers->ci = 1 << slot;

        for(;;) {
                if (!(registers->ci & (1 << slot))) 
                        break;
                if (registers->is & HBA_PxIS_TFES)   // Task file error
                {
                    Log::Warning("[SATA] Disk Error");
                    return 1;
                }
        }
        
        if (registers->is & HBA_PxIS_TFES) {
            Log::Warning("[SATA] Disk Error");
            return 1;
        }

        stopCMD(registers);

        return 0;
    }

    int Port::FindCmdSlot(){
        // If not set in SACT and CI, the slot is free
        uint32_t slots = (registers->sact | registers->ci);
        for (int i=0; i<8; i++)
        {
            if ((slots&1) == 0)
                return i;
            slots >>= 1;
        }

        return -1;
    }
}


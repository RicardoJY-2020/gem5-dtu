/*
 * Copyright (c) 2015, Nils Asmussen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include "arch/x86/m3/system.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/isa_traits.hh"
#include "arch/vtophys.hh"
#include "base/trace.hh"
#include "base/loader/object_file.hh"
#include "cpu/thread_context.hh"
#include "debug/DtuTlb.hh"
#include "mem/port_proxy.hh"
#include "mem/dtu/pt_unit.hh"
#include "mem/dtu/tlb.hh"
#include "params/M3X86System.hh"
#include "sim/byteswap.hh"

#include <libgen.h>

using namespace LittleEndianGuest;
using namespace X86ISA;

const unsigned M3X86System::RES_PAGES =
    (STACK_AREA + STACK_SIZE) >> DtuTlb::PAGE_BITS;

M3X86System::M3X86System(Params *p)
    : X86System(p),
      commandLine(p->boot_osflags),
      coreId(p->core_id),
      memPe(p->memory_pe),
      memOffset(p->memory_offset),
      memSize(p->memory_size),
      modOffset(p->mod_offset),
      // don't reuse root pt
      nextFrame(RES_PAGES)
{
}

M3X86System::~M3X86System()
{
}

size_t
M3X86System::getArgc() const
{
    const char *cmd = commandLine.c_str();
    size_t argc = 0;
    size_t len = 0;
    while (*cmd)
    {
        if (isspace(*cmd))
        {
            if(len > 0)
                argc++;
            len = 0;
        }
        else
            len++;
        cmd++;
    }
    if(len > 0)
        argc++;

    return argc;
}

void
M3X86System::writeArg(Addr &args, size_t &i, Addr argv, const char *cmd, const char *begin) const
{
    const char zero[] = {0};
    // write argument pointer
    uint64_t argvPtr = args;
    physProxy.writeBlob(argv + i * sizeof(uint64_t), (uint8_t*)&argvPtr, sizeof(argvPtr));
    // write argument
    physProxy.writeBlob(args, (uint8_t*)begin, cmd - begin);
    args += cmd - begin;
    physProxy.writeBlob(args, (uint8_t*)zero, 1);
    args++;
    i++;
}

Addr
M3X86System::loadModule(const std::string &path, const std::string &name, Addr addr) const
{
    std::string filename = path + "/" + name;
    FILE *f = fopen(filename.c_str(), "r");
    if(!f)
        panic("Unable to open '%s' for reading", filename.c_str());

    fseek(f, 0L, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0L, SEEK_SET);

    auto data = new uint8_t[sz];
    if(fread(data, 1, sz, f) != sz)
        panic("Unable to read '%s'", filename.c_str());
    physProxy.writeBlob(addr, data, sz);
    delete[] data;
    fclose(f);

    return sz;
}

void
M3X86System::mapPage(Addr virt, Addr phys, uint access)
{
    typedef PtUnit::PageTableEntry pte_t;
    Addr ptAddr = getRootPt().getAddr();
    for (int i = DtuTlb::LEVEL_CNT - 1; i >= 0; --i)
    {
        Addr idx = virt >> (DtuTlb::PAGE_BITS + i * DtuTlb::LEVEL_BITS);
        idx &= DtuTlb::LEVEL_MASK;

        Addr pteAddr = ptAddr + (idx << DtuTlb::PTE_BITS);
        pte_t entry = physProxy.read<pte_t>(pteAddr);
        assert(i > 0 || entry.ixwr == 0);
        if(!entry.ixwr)
        {
            // determine phys address
            Addr offset;
            if (i == 0)
                offset = memOffset + phys;
            else
                offset = memOffset + (nextFrame++ << DtuTlb::PAGE_BITS);
            NocAddr addr(memPe, 0, offset);

            // clear pagetables
            if (i > 0)
                physProxy.memsetBlob(addr.getAddr(), 0, DtuTlb::PAGE_SIZE);

            // insert entry
            entry.base = addr.getAddr() >> DtuTlb::PAGE_BITS;
            entry.ixwr = i == 0 ? access : DtuTlb::RWX;
            DPRINTF(DtuTlb,
                "Creating level %d PTE for virt=%#018x @ %#018x: %#018x\n",
                i, virt, pteAddr, entry);
            physProxy.write(pteAddr, entry);
        }

        ptAddr = entry.base << DtuTlb::PAGE_BITS;
    }
}

void
M3X86System::mapSegment(Addr start, Addr size, unsigned perm)
{
    Addr virt = start;
    size_t count = divCeil(size, DtuTlb::PAGE_SIZE);
    for(size_t i = 0; i < count; ++i)
    {
        mapPage(virt, virt, perm);

        virt += DtuTlb::PAGE_SIZE;
    }
}

void
M3X86System::mapMemory()
{
    // clear root pt
    physProxy.memsetBlob(getRootPt().getAddr(), 0, DtuTlb::PAGE_SIZE);

    // let the last entry in the root pt point to the root pt itself
    PtUnit::PageTableEntry entry = 0;
    entry.base = getRootPt().getAddr() >> DtuTlb::PAGE_BITS;
    // not internally accessible
    entry.ixwr = DtuTlb::RWX;
    size_t off = DtuTlb::PAGE_SIZE - sizeof(entry);
    DPRINTF(DtuTlb,
        "Creating recursive level %d PTE @ %#018x: %#018x\n",
        DtuTlb::LEVEL_CNT - 1, getRootPt().getAddr() + off, entry);
    physProxy.write(getRootPt().getAddr() + off, entry);

    // program segments
    mapSegment(kernel->textBase(), kernel->textSize(),
        DtuTlb::INTERN | DtuTlb::RX);
    mapSegment(kernel->dataBase(), kernel->dataSize(),
        DtuTlb::INTERN | DtuTlb::RW);
    mapSegment(kernel->bssBase(), kernel->bssSize(),
        DtuTlb::INTERN | DtuTlb::RW);

    // idle doesn't need that stuff
    if (modOffset)
    {
        // initial heap
        Addr bssEnd = roundUp(kernel->bssBase() + kernel->bssSize(),
            DtuTlb::PAGE_SIZE);
        mapSegment(bssEnd, HEAP_SIZE, DtuTlb::INTERN | DtuTlb::RW);

        // state and stack
        mapSegment(RT_START, RT_SIZE, DtuTlb::INTERN | DtuTlb::RW);
        mapSegment(STACK_AREA, STACK_SIZE, DtuTlb::INTERN | DtuTlb::RW);
    }
    else
    {
        // map a large portion of the address space on app PEs
        // TODO this is temporary to still support clone and VPEs without AS
        mapSegment(0, memSize, DtuTlb::IRWX);
    }
}

void
M3X86System::initState()
{
    X86System::initState();

    mapMemory();

    StartEnv env;
    memset(&env, 0, sizeof(env));
    env.coreid = coreId;
    env.argc = getArgc();
    Addr argv = RT_START + sizeof(env);
    Addr args = argv + sizeof(void*) * env.argc;
    env.argv = reinterpret_cast<char**>(argv);

    // check if there is enough space
    if (commandLine.length() + 1 > RT_START + RT_SIZE - args)
    {
        panic("Command line \"%s\" is longer than %d characters.\n",
                commandLine, RT_START + RT_SIZE - args - 1);
    }

    std::string kernelPath;
    std::string prog;
    std::string argstr;
    std::vector<std::pair<std::string,std::string>> mods;

    // write arguments to state area and determine boot modules
    const char *cmd = commandLine.c_str();
    const char *begin = cmd;
    size_t i = 0;
    while (*cmd)
    {
        if (isspace(*cmd))
        {
            if (cmd > begin)
            {
                // the first is the kernel; remember the path
                if (i == 0)
                {
                    std::string path(begin, cmd - begin);
                    char *copy = strdup(path.c_str());
                    kernelPath = dirname(copy);
                    free(copy);
                }
                else if (modOffset)
                {
                    if (strncmp(begin, "--", 2) == 0)
                    {
                        mods.push_back(std::make_pair(prog, argstr));
                        prog = "";
                        argstr = "";
                    }
                    else if (prog.empty())
                        prog = std::string(begin, cmd - begin);
                    else
                    {
                        if (!argstr.empty())
                            argstr += ' ';
                        argstr += std::string(begin, cmd - begin);
                    }
                }

                writeArg(args, i, argv, cmd, begin);
            }
            begin = cmd + 1;
        }
        cmd++;
    }

    if (cmd > begin)
    {
        if (prog.empty())
            prog = std::string(begin, cmd - begin);
        else
        {
            if (!argstr.empty())
                argstr += ' ';
            argstr += std::string(begin, cmd - begin);
        }

        mods.push_back(std::make_pair(prog, argstr));

        writeArg(args, i, argv, cmd, begin);
    }

    if (modOffset && mods.size() > 0)
    {
        // idle is always needed
        mods.push_back(std::make_pair("idle", ""));

        if(mods.size() > MAX_MODS)
            panic("Too many modules");

        i = 0;
        Addr addr = NocAddr(memPe, 0, modOffset).getAddr();
        for (const std::pair<std::string, std::string> &mod : mods)
        {
            Addr size = loadModule(kernelPath, mod.first, addr);

            // construct module info
            BootModule bmod;
            size_t cmdlen = mod.first.length() + mod.second.length() + 1;
            if(cmdlen >= sizeof(bmod.name))
                panic("Module name too long: %s", mod.first.c_str());
            strcpy(bmod.name, mod.first.c_str());
            if (!mod.second.empty())
            {
                strcat(bmod.name, " ");
                strcat(bmod.name, mod.second.c_str());
            }
            bmod.addr = addr;
            bmod.size = size;

            inform("Loaded '%s' to %p .. %p",
                bmod.name, bmod.addr, bmod.addr + bmod.size);

            // store pointer to area module info and info itself
            env.mods[i] = roundUp(addr + size, sizeof(uint64_t));
            physProxy.writeBlob(env.mods[i],
                reinterpret_cast<uint8_t*>(&bmod), sizeof(bmod));

            // to next
            addr = env.mods[i] + sizeof(bmod);
            addr += DtuTlb::PAGE_SIZE - 1;
            addr &= ~static_cast<Addr>(DtuTlb::PAGE_SIZE - 1);
            i++;
        }

        // termination
        env.mods[i] = 0;
    }

    // write env
    physProxy.writeBlob(RT_START, reinterpret_cast<uint8_t*>(&env), sizeof(env));
}

M3X86System *
M3X86SystemParams::create()
{
    return new M3X86System(this);
}

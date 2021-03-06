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

#include "sim/m3_loader.hh"
#include "arch/vtophys.hh"
#include "base/trace.hh"
#include "base/loader/object_file.hh"
#include "cpu/thread_context.hh"
#include "debug/DtuTlb.hh"
#include "mem/port_proxy.hh"
#include "mem/dtu/pt_unit.hh"
#include "mem/dtu/tlb.hh"
#include "mem/dtu/dtu.hh"
#include "sim/byteswap.hh"

#include <libgen.h>

const unsigned M3Loader::RES_PAGES =
    (STACK_AREA + STACK_SIZE) >> DtuTlb::PAGE_BITS;

M3Loader::M3Loader(const std::vector<Addr> &pes,
                   const std::string &cmdline,
                   unsigned coreId,
                   Addr modOffset,
                   Addr modSize)
    : pes(pes),
      commandLine(cmdline),
      coreId(coreId),
      modOffset(modOffset),
      modSize(modSize)
{
}

size_t
M3Loader::getArgc() const
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

bool
M3Loader::isKernelArg(const std::string &arg)
{
    if (arg == "daemon")
        return true;
    if (arg == "muxable")
        return true;
    if (arg.find("requires=") == 0)
        return true;
    if (arg.find("core=") == 0)
        return true;
    return false;
}

void
M3Loader::writeArg(System &sys, Addr &args, size_t &i, Addr argv, const char *cmd, const char *begin)
{
    const char zero[] = {0};
    // write argument pointer
    uint64_t argvPtr = args;
    sys.physProxy.writeBlob(argv + i * sizeof(uint64_t), (uint8_t*)&argvPtr, sizeof(argvPtr));
    // write argument
    sys.physProxy.writeBlob(args, (uint8_t*)begin, cmd - begin);
    args += cmd - begin;
    sys.physProxy.writeBlob(args, (uint8_t*)zero, 1);
    args++;
    i++;
}

void
M3Loader::writeRemote(MasterPort &noc, Addr dest, const uint8_t *data, size_t size)
{
    RequestPtr req = std::make_shared<Request>(dest, size, 0, Request::funcMasterId);
    Packet pkt(req, MemCmd::WriteReq);
    pkt.dataStaticConst(data);

    auto senderState = new Dtu::NocSenderState();
    senderState->packetType = Dtu::NocPacketType::CACHE_MEM_REQ_FUNC;
    senderState->result = Dtu::Error::NONE;

    pkt.pushSenderState(senderState);

    noc.sendFunctional(&pkt);

    delete senderState;
}

Addr
M3Loader::loadModule(MasterPort &noc, const std::string &path, const std::string &name, Addr addr)
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
    writeRemote(noc, addr, data, sz);
    delete[] data;
    fclose(f);

    return sz;
}

void
M3Loader::mapMemory(System &sys, DTUMemory &dtumem)
{
    dtumem.initMemory();
    // TODO check whether the size of idle fits before the RT_SPACE

    // program segments
    dtumem.mapSegment(sys.kernel->textBase(), sys.kernel->textSize(),
        DtuTlb::INTERN | DtuTlb::RX);
    dtumem.mapSegment(sys.kernel->dataBase(), sys.kernel->dataSize(),
        DtuTlb::INTERN | DtuTlb::RW);
    dtumem.mapSegment(sys.kernel->bssBase(), sys.kernel->bssSize(),
        DtuTlb::INTERN | DtuTlb::RW);

    // idle doesn't need that stuff
    if (modOffset)
    {
        // initial heap
        Addr bssEnd = roundUp(sys.kernel->bssBase() + sys.kernel->bssSize(),
            DtuTlb::PAGE_SIZE);
        dtumem.mapSegment(bssEnd, HEAP_SIZE, DtuTlb::INTERN | DtuTlb::RW);

        // state and stack
        dtumem.mapSegment(RT_START, RT_SIZE, DtuTlb::INTERN | DtuTlb::RW);
        dtumem.mapSegment(STACK_AREA, STACK_SIZE, DtuTlb::INTERN | DtuTlb::RW);
    }
}

void
M3Loader::initState(System &sys, DTUMemory &dtumem, MasterPort &noc)
{
    // external memory? then we use paging
    if ((pes[coreId] & 0x7) == 1)
        mapMemory(sys, dtumem);

    StartEnv env;
    memset(&env, 0, sizeof(env));
    env.coreid = coreId;
    env.argc = getArgc();
    Addr argv = RT_START + sizeof(env);
    // the kernel gets the kernel env behind the normal env
    if (modOffset)
        argv += sizeof(KernelEnv);
    Addr args = argv + sizeof(uint64_t) * env.argc;
    env.argv = argv;

    // with paging, the kernel gets an initial heap mapped
    if ((pes[coreId] & 0x7) == 1 || (pes[coreId] & 0x7) == 2)
        env.heapsize = HEAP_SIZE;
    // otherwise, he should use all internal memory
    else
        env.heapsize = 0;

    // check if there is enough space
    if (commandLine.length() + 1 > RT_START + RT_SIZE - args)
    {
        panic("Command line \"%s\" is longer than %d characters.\n",
                commandLine, RT_START + RT_SIZE - args - 1);
    }

    if (pes.size() > MAX_PES)
    {
        const size_t max = MAX_PES;
        panic("Too many PEs (%u vs. %u)", pes.size(), max);
    }

    std::string kernelPath;
    std::string prog;
    std::string argstr;
    std::vector<std::pair<std::string,std::string>> mods;

    // write arguments to state area and determine boot modules
    const char *cmd = commandLine.c_str();
    const char *begin = cmd;
    bool seen_dashes = false;
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
                    if(!seen_dashes)
                    {
                        if (strncmp(begin, "--", 2) == 0)
                            seen_dashes = true;
                    }
                    else if (strncmp(begin, "--", 2) == 0)
                    {
                        mods.push_back(std::make_pair(prog, argstr));
                        prog = "";
                        argstr = "";
                    }
                    else if (prog.empty())
                        prog = std::string(begin, cmd - begin);
                    else
                    {
                        std::string arg(begin, cmd - begin);
                        if (!isKernelArg(arg))
                        {
                            if (!argstr.empty())
                                argstr += ' ';
                            argstr += arg;
                        }
                    }
                }

                writeArg(sys, args, i, argv, cmd, begin);
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
            std::string arg(begin, cmd - begin);
            if (!isKernelArg(arg))
            {
                if (!argstr.empty())
                    argstr += ' ';
                argstr += arg;
            }
        }

        mods.push_back(std::make_pair(prog, argstr));

        writeArg(sys, args, i, argv, cmd, begin);
    }

    // modules for the kernel
    if (modOffset)
    {
        KernelEnv kenv;

        // rctmux is always needed
        mods.push_back(std::make_pair("rctmux", ""));

        if (mods.size() > MAX_MODS)
            panic("Too many modules");

        i = 0;
        Addr addr = NocAddr(dtumem.memPe, modOffset).getAddr();
        for (const std::pair<std::string, std::string> &mod : mods)
        {
            Addr size = loadModule(noc, kernelPath, mod.first, addr);

            // construct module info
            BootModule bmod;
            size_t cmdlen = mod.first.length() + mod.second.length() + 1;
            if (cmdlen >= sizeof(bmod.name))
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
            kenv.mods[i] = roundUp(addr + size, sizeof(uint64_t));
            writeRemote(noc, kenv.mods[i],
                reinterpret_cast<uint8_t*>(&bmod), sizeof(bmod));

            // to next
            addr = kenv.mods[i] + sizeof(bmod);
            addr += DtuTlb::PAGE_SIZE - 1;
            addr &= ~static_cast<Addr>(DtuTlb::PAGE_SIZE - 1);
            i++;
        }

        // termination
        kenv.mods[i] = 0;

        // build PE array
        kenv.pe_count = pes.size();
        memset(kenv.pes, 0, sizeof(kenv.pes));
        for (size_t i = 0; i < pes.size(); ++i)
            kenv.pes[i] = pes[i];

        // the kernel needs to PE info in its env
        env.pe = kenv.pes[coreId];

        // write kenv
        env.kenv = addr;
        writeRemote(noc, env.kenv, reinterpret_cast<uint8_t*>(&kenv), sizeof(kenv));
        addr += sizeof(kenv);

        // check size
        Addr end = NocAddr(dtumem.memPe, modOffset + modSize).getAddr();
        if (addr > end)
        {
            panic("Modules are too large (have: %lu, need: %lu)",
                modSize, addr - NocAddr(dtumem.memPe, modOffset).getAddr());
        }
    }

    // write env
    sys.physProxy.writeBlob(
        RT_START, reinterpret_cast<uint8_t*>(&env), sizeof(env));
}

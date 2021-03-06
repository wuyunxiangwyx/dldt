// Copyright (C) 2018 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0
//

#include "lin_omp_manager.h"

#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <iostream>

namespace MKLDNNPlugin {
namespace cpu {

#ifndef __APPLE__

Processor::Processor() {
    processor = 0;
    physicalId = 0;
    siblings = 0;
    coreId = 0;
    cpuCores = 0;
    speedMHz = 0;
}

CpuInfo::CpuInfo() {
    loadContentFromFile("/proc/cpuinfo");
}

CpuInfo::CpuInfo(const char *content) {
    loadContent(content);
}

void CpuInfo::loadContentFromFile(const char *fileName) {
    std::ifstream file(fileName);
    std::string content(
            (std::istreambuf_iterator<char>(file)),
            (std::istreambuf_iterator<char>()));

    loadContent(content.c_str());
}

void CpuInfo::loadContent(const char *content) {
    size_t contentLength = strlen(content);
    char *contentCopy = new char[contentLength + 1];
    snprintf(contentCopy, contentLength + 1, "%s", content);

    parseLines(contentCopy);

    fileContentBegin = contentCopy;
    fileContentEnd = &contentCopy[contentLength];
    currentLine = NULL;
}

CpuInfo::~CpuInfo() {
    delete[] fileContentBegin;
}

void CpuInfo::parseLines(char *content) {
    for (; *content; content++) {
        if (*content == '\n') {
            *content = '\0';
        }
    }
}

const char *CpuInfo::getFirstLine() {
    currentLine = fileContentBegin < fileContentEnd ? fileContentBegin : NULL;
    return getNextLine();
}

const char *CpuInfo::getNextLine() {
    if (!currentLine) {
        return NULL;
    }

    const char *savedCurrentLine = currentLine;
    while (*(currentLine++)) {
    }

    if (currentLine >= fileContentEnd) {
        currentLine = NULL;
    }

    return savedCurrentLine;
}

Collection::Collection(CpuInfoInterface *cpuInfo) : cpuInfo(*cpuInfo) {
    totalNumberOfSockets = 0;
    totalNumberOfCpuCores = 0;
    currentProcessor = NULL;

    processors.reserve(96);

    parseCpuInfo();
    collectBasicCpuInformation();
}

unsigned Collection::getProcessorSpeedMHz() {
    return processors.size() ? processors[0].speedMHz : 0;
}

unsigned Collection::getTotalNumberOfSockets() {
    return totalNumberOfSockets;
}

unsigned Collection::getTotalNumberOfCpuCores() {
    return totalNumberOfCpuCores;
}

unsigned Collection::getNumberOfProcessors() {
    return processors.size();
}

const Processor &Collection::getProcessor(unsigned processorId) {
    return processors[processorId];
}

void Collection::parseCpuInfo() {
    const char *cpuInfoLine = cpuInfo.getFirstLine();
    for (; cpuInfoLine; cpuInfoLine = cpuInfo.getNextLine()) {
        parseCpuInfoLine(cpuInfoLine);
    }
}

void Collection::parseCpuInfoLine(const char *cpuInfoLine) {
    int delimiterPosition = strcspn(cpuInfoLine, ":");

    if (cpuInfoLine[delimiterPosition] == '\0') {
        currentProcessor = NULL;
    } else {
        parseValue(cpuInfoLine, &cpuInfoLine[delimiterPosition + 2]);
    }
}

void Collection::parseValue(const char *fieldName, const char *valueString) {
    if (!currentProcessor) {
        appendNewProcessor();
    }

    if (beginsWith(fieldName, "processor")) {
        currentProcessor->processor = parseInteger(valueString);
    }

    if (beginsWith(fieldName, "physical id")) {
        currentProcessor->physicalId = parseInteger(valueString);
    }

    if (beginsWith(fieldName, "siblings")) {
        currentProcessor->siblings = parseInteger(valueString);
    }

    if (beginsWith(fieldName, "core id")) {
        currentProcessor->coreId = parseInteger(valueString);
    }

    if (beginsWith(fieldName, "cpu cores")) {
        currentProcessor->cpuCores = parseInteger(valueString);
    }

    if (beginsWith(fieldName, "model name")) {
        currentProcessor->speedMHz = extractSpeedFromModelName(valueString);
    }
}

void Collection::appendNewProcessor() {
    processors.push_back(Processor());
    currentProcessor = &processors.back();
}

bool Collection::beginsWith(const char *lineBuffer, const char *text) const {
    while (*text) {
        if (*(lineBuffer++) != *(text++)) {
            return false;
        }
    }

    return true;
}

unsigned Collection::parseInteger(const char *text) const {
    return atol(text);
}

/* Function extracts CPU speed from model name. If unit is not set it is
   assumed that values below 100 are specified in GHz, otherwise MHz */
unsigned Collection::extractSpeedFromModelName(const char *text) const {
    text = strstr(text, "@");
    if (!text) {
        return 0;
    }

    char *unit;
    double speed = strtod(&text[1], &unit);

    while (isspace(*unit)) {
        unit++;
    }

    bool isMHz = !strncmp(unit, "MHz", 3);
    bool isGHz = !strncmp(unit, "GHz", 3);
    bool isGHzPossible = (speed < 100);

    if (isGHz || (isGHzPossible && !isMHz)) {
        return 1000 * speed + 0.5;
    } else {
        return speed + 0.5;
    }
}

void Collection::collectBasicCpuInformation() {
    std::set<unsigned> uniquePhysicalId;
    std::vector<Processor>::iterator processor = processors.begin();
    for (; processor != processors.end(); processor++) {
        uniquePhysicalId.insert(processor->physicalId);
        updateCpuInformation(*processor, uniquePhysicalId.size());
    }
}

void Collection::updateCpuInformation(const Processor &processor,
                                      unsigned numberOfUniquePhysicalId) {
    if (totalNumberOfSockets == numberOfUniquePhysicalId) {
        return;
    }

    totalNumberOfSockets = numberOfUniquePhysicalId;
    totalNumberOfCpuCores += processor.cpuCores;
}


/* The OpenMpManager class is responsible for determining a set of all of
   available CPU cores and delegating each core to perform other tasks. The
   first of available cores is delegated for background threads, while other
   remaining cores are dedicated for OpenMP threads. Each OpenMP thread owns
   one core for exclusive use. The number of OpenMP threads is then limited
   to the number of available cores minus one. The amount of CPU cores may
   be limited by system eg. when numactl was used. */

#include <omp.h>
#include <sched.h>

static const char *openMpEnvVars[] = {
        "OMP_CANCELLATION", "OMP_DISPLAY_ENV", "OMP_DEFAULT_DEVICE", "OMP_DYNAMIC",
        "OMP_MAX_ACTIVE_LEVELS", "OMP_MAX_TASK_PRIORITY", "OMP_NESTED",
        "OMP_NUM_THREADS", "OMP_PROC_BIND", "OMP_PLACES", "OMP_STACKSIZE",
        "OMP_SCHEDULE", "OMP_THREAD_LIMIT", "OMP_WAIT_POLICY", "GOMP_CPU_AFFINITY",
        "GOMP_DEBUG", "GOMP_STACKSIZE", "GOMP_SPINCOUNT", "GOMP_RTEMS_THREAD_POOLS",
        "KMP_AFFINITY", "KMP_NUM_THREADS", "MIC_KMP_AFFINITY",
        "MIC_OMP_NUM_THREADS", "MIC_OMP_PROC_BIND", "PHI_KMP_AFFINITY",
        "PHI_OMP_NUM_THREADS", "PHI_KMP_PLACE_THREADS", "MKL_NUM_THREADS",
        "MKL_DYNAMIC", "MKL_DOMAIN_NUM_THREADS"
};

static const unsigned numberOfOpenMpEnvVars =
        sizeof(openMpEnvVars) / sizeof(openMpEnvVars[0]);

OpenMpManager::OpenMpManager(Collection *collection) :
        collection(*collection), isGpuEnabled(false) {
    getOpenMpEnvVars();
    getCurrentCpuSet();
    getCurrentCoreSet();
}

OpenMpManager &OpenMpManager::getInstance() {
    static CpuInfo cpuInfo;
    static Collection collection(&cpuInfo);
    static OpenMpManager openMpManager(&collection);
    return openMpManager;
}

void OpenMpManager::setGpuEnabled() {
    OpenMpManager &openMpManager = getInstance();
    openMpManager.isGpuEnabled = true;
}

void OpenMpManager::setGpuDisabled() {
    OpenMpManager &openMpManager = getInstance();
    openMpManager.isGpuEnabled = false;
}

// Ideally bind given thread to secondary logical core, if
// only one thread exists then bind to primary one
void OpenMpManager::bindCurrentThreadToNonPrimaryCoreIfPossible() {
    OpenMpManager &openMpManager = getInstance();
    if (openMpManager.isThreadsBindAllowed()) {
        int totalNumberOfAvailableCores = CPU_COUNT(&openMpManager.currentCoreSet);
        int logicalCoreToBindTo = totalNumberOfAvailableCores > 1 ? 1 : 0;
        openMpManager.bindCurrentThreadToLogicalCoreCpus(logicalCoreToBindTo);
    }
}

void OpenMpManager::bindOpenMpThreads(int env_cores) {
    OpenMpManager &openMpManager = getInstance();

    if (!openMpManager.isThreadsBindAllowed())
        return;

    openMpManager.setOpenMpThreadNumberLimit(env_cores);
    #pragma omp parallel
    {
        unsigned logicalCoreId = omp_get_thread_num();
        openMpManager.bindCurrentThreadToLogicalCoreCpu(logicalCoreId);
    }
}

int OpenMpManager::getOpenMpThreadNumber() {
    OpenMpManager &openMpManager = getInstance();

    return openMpManager.getCoreNumber();
}


void OpenMpManager::getOpenMpEnvVars() {
    isAnyOpenMpEnvVarSpecified = false;
    for (unsigned i = 0; i < numberOfOpenMpEnvVars; i++) {
        if (getenv(openMpEnvVars[i])) {
            isAnyOpenMpEnvVarSpecified = true;
        }
    }
}

void OpenMpManager::getCurrentCpuSet() {
    if (sched_getaffinity(0, sizeof(currentCpuSet), &currentCpuSet)) {
        getDefaultCpuSet(&currentCpuSet);
    }
}

void OpenMpManager::getDefaultCpuSet(cpu_set_t *defaultCpuSet) {
    CPU_ZERO(defaultCpuSet);
    unsigned numberOfProcessors = collection.getNumberOfProcessors();
    for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
        CPU_SET(processorId, defaultCpuSet);
    }
}

/* Function getCurrentCoreSet() fills currentCoreSet variable with a set of
   available CPUs, where only one CPU per core is chosen. When multiple CPUs
   of single core are used, function is selecting only first one of all
   available. */
void OpenMpManager::getCurrentCoreSet() {
    unsigned numberOfProcessors = collection.getNumberOfProcessors();
    unsigned totalNumberOfCpuCores = collection.getTotalNumberOfCpuCores();

    cpu_set_t usedCoreSet;
    CPU_ZERO(&usedCoreSet);
    CPU_ZERO(&currentCoreSet);

    for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
        if (CPU_ISSET(processorId, &currentCpuSet)) {
            unsigned coreId = processorId % totalNumberOfCpuCores;
            if (!CPU_ISSET(coreId, &usedCoreSet)) {
                CPU_SET(coreId, &usedCoreSet);
                CPU_SET(processorId, &currentCoreSet);
            }
        }
    }
}

void OpenMpManager::selectAllCoreCpus(cpu_set_t *set, unsigned physicalCoreId) {
    unsigned numberOfProcessors = collection.getNumberOfProcessors();
    unsigned totalNumberOfCpuCores = collection.getTotalNumberOfCpuCores();

    int processorId = physicalCoreId % totalNumberOfCpuCores;
    while (processorId < numberOfProcessors) {
        if (CPU_ISSET(processorId, &currentCpuSet)) {
            CPU_SET(processorId, set);
        }

        processorId += totalNumberOfCpuCores;
    }
}

unsigned OpenMpManager::getPhysicalCoreId(unsigned logicalCoreId) {
    unsigned numberOfProcessors = collection.getNumberOfProcessors();

    for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
        if (CPU_ISSET(processorId, &currentCoreSet)) {
            if (!logicalCoreId--) {
                return processorId;
            }
        }
    }

    std::cerr << "This should never happen!";
    return 0;
}

bool OpenMpManager::isThreadsBindAllowed() {
    return !isAnyOpenMpEnvVarSpecified && !isGpuEnabled;
}

// Limit of threads to number of logical cores available
void OpenMpManager::setOpenMpThreadNumberLimit(int env_cores) {
    omp_set_num_threads(env_cores == 0 ? CPU_COUNT(&currentCoreSet) : 0);
}

int OpenMpManager::getCoreNumber() {
    return CPU_COUNT(&currentCoreSet);
}

void OpenMpManager::bindCurrentThreadToLogicalCoreCpu(unsigned logicalCoreId) {
    unsigned physicalCoreId = getPhysicalCoreId(logicalCoreId);

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(physicalCoreId, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

void OpenMpManager::bindCurrentThreadToLogicalCoreCpus(unsigned logicalCoreId) {
    unsigned physicalCoreId = getPhysicalCoreId(logicalCoreId);

    cpu_set_t set;
    CPU_ZERO(&set);
    selectAllCoreCpus(&set, physicalCoreId);
    sched_setaffinity(0, sizeof(set), &set);
}

#endif  // #ifndef APPLE
}  // namespace cpu
}  // namespace MKLDNNPlugin

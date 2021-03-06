/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <string>
#include <vector>
#include <memory>

#include "ClassificationProcessor.hpp"
#include "Processor.hpp"

using InferenceEngine::details::InferenceEngineException;

ClassificationProcessor::ClassificationProcessor(const std::string& flags_m, const std::string& flags_d, const std::string& flags_i, int flags_b,
        InferencePlugin plugin, CsvDumper& dumper, const std::string& flags_l, PreprocessingOptions preprocessingOptions, bool zeroBackground)
    : Processor(flags_m, flags_d, flags_i, flags_b, plugin, dumper, "Classification network", preprocessingOptions), zeroBackground(zeroBackground) {

    // Change path to labels file if necessary
    if (flags_l.empty()) {
        labelFileName = fileNameNoExt(modelFileName) + ".labels";
    } else {
        labelFileName = flags_l;
    }
}

ClassificationProcessor::ClassificationProcessor(const std::string& flags_m, const std::string& flags_d, const std::string& flags_i, int flags_b,
        InferencePlugin plugin, CsvDumper& dumper, const std::string& flags_l, bool zeroBackground)
    : ClassificationProcessor(flags_m, flags_d, flags_i, flags_b, plugin, dumper, flags_l,
            PreprocessingOptions(false, ResizeCropPolicy::ResizeThenCrop, 256, 256), zeroBackground) {
}

std::shared_ptr<Processor::InferenceMetrics> ClassificationProcessor::Process() {
     slog::info << "Collecting labels" << slog::endl;
     ClassificationSetGenerator generator;
     // try {
     //     generator.readLabels(labelFileName);
     // } catch (InferenceEngine::details::InferenceEngineException& ex) {
     //     slog::warn << "Can't read labels file " << labelFileName << slog::endl;
     // }

     auto validationMap = generator.getValidationMap(imagesPath);
     ImageDecoder decoder;

     // ----------------------------Do inference-------------------------------------------------------------
     slog::info << "Starting inference" << slog::endl;

     std::vector<int> expected(batch);
     std::vector<std::string> files(batch);
     int captured = 0;

     ConsoleProgress progress(validationMap.size());

     ClassificationInferenceMetrics im;

     std::string firstInputName = this->inputInfo.begin()->first;
     std::string firstOutputName = this->outInfo.begin()->first;
     auto firstInputBlob = inferRequest.GetBlob(firstInputName);
     auto firstOutputBlob = inferRequest.GetBlob(firstOutputName);

     auto iter = validationMap.begin();
     while (iter != validationMap.end()) {
         int b = 0;
         int filesWatched = 0;
         for (; b < batch && iter != validationMap.end(); b++, iter++, filesWatched++) {
             expected[b] = iter->first;
             try {
                 decoder.insertIntoBlob(iter->second, b, *firstInputBlob, preprocessingOptions);
                 files[b] = iter->second;
             } catch (const InferenceEngineException& iex) {
                 slog::warn << "Can't read file " << iter->second << slog::endl;
                 // Could be some non-image file in directory
                 b--;
                 continue;
             }
         }

         Infer(progress, filesWatched, im);

         std::vector<unsigned> results;
         auto firstOutputData = firstOutputBlob->buffer().as<PrecisionTrait<Precision::FP32>::value_type*>();
         InferenceEngine::TopResults(TOP_COUNT, *firstOutputBlob, results);

         for (int i = 0; i < b; i++) {
             int expc = expected[i];
             if (zeroBackground) expc++;

             bool top1Scored = (results[0 + TOP_COUNT * i] == expc);
             dumper << "\"" + files[i] + "\"" << top1Scored;
             if (top1Scored) im.top1Result++;
             for (int j = 0; j < TOP_COUNT; j++) {
                 unsigned classId = results[j + TOP_COUNT * i];
                 if (classId == expc) {
                     im.topCountResult++;
                 }
                 dumper << classId << firstOutputData[classId + i * (firstOutputBlob->size() / batch)];
             }
             dumper.endLine();
             im.total++;
         }
     }
     progress.finish();

     return std::shared_ptr<Processor::InferenceMetrics>(new ClassificationInferenceMetrics(im));
}

void ClassificationProcessor::Report(const Processor::InferenceMetrics& im) {
    Processor::Report(im);
    if (im.nRuns > 0) {
        const ClassificationInferenceMetrics& cim = dynamic_cast<const ClassificationInferenceMetrics&>(im);

        cout << "Top1 accuracy: " << OUTPUT_FLOATING(100.0 * cim.top1Result / cim.total) << "% (" << cim.top1Result << " of "
                << cim.total << " images were detected correctly, top class is correct)" << "\n";
        cout << "Top5 accuracy: " << OUTPUT_FLOATING(100.0 * cim.topCountResult / cim.total) << "% (" << cim.topCountResult << " of "
            << cim.total << " images were detected correctly, top five classes contain required class)" << "\n";
    }
}


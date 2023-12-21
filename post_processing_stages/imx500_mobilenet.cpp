/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2023, Raspberry Pi Ltd
 *
 * imx500_mobilenet.cpp - IMX500 inference for MobileNet SSD
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#include <libcamera/control_ids.h>
#include <libcamera/stream.h>

#include "core/rpicam_app.hpp"
#include "post_processing_stages/object_detect.hpp"
#include "post_processing_stages/post_processing_stage.hpp"

#include "apParams.flatbuffers_generated.h"

using Stream = libcamera::Stream;
namespace controls = libcamera::controls;

// Derived from SSDMobilnetV1 DNN Model
static constexpr unsigned int PplTotalDetections = 10;
// bbox(10*4)+class(10)+scores(10)+numDetections(1) = 61
static constexpr unsigned int PplDnnOutputTensorSize = 61;

enum TensorDataType
{
	TYPE_SIGNED = 0,
	TYPE_UNSIGNED
};

struct DnnHeader
{
	uint8_t frameValid;
	uint8_t frameCount;
	uint16_t maxLineLen;
	uint16_t apParamSize;
	uint16_t networkId;
	uint8_t tensorType;
};

struct Dimensions
{
	uint8_t ordinal;
	uint16_t size;
	uint8_t serializationIndex;
	uint8_t padding;
};

struct OutputTensorApParams
{
	uint8_t id;
	char *name;
	uint16_t numDimensions;
	uint8_t bitsPerElement;
	std::vector<Dimensions> vecDim;
	uint16_t shift;
	float scale;
	uint8_t format;
};

struct OutputTensorInfo
{
	std::vector<float> address;
	size_t totalSize;
	uint32_t tensorNum;
	std::vector<uint32_t> tensorDataNum;
};

// MobileNet SSD specific structures
struct Bbox
{
	float xMin;
	float yMin;
	float xMax;
	float yMax;
};

struct ObjectDetectionSsdOutputTensor
{
	unsigned int numDetections = 0;
	std::vector<Bbox> bboxes;
	std::vector<float> scores;
	std::vector<float> classes;
};

struct PplBbox
{
	uint16_t mXmin;
	uint16_t mYmin;
	uint16_t mXmax;
	uint16_t mYmax;
};

struct ObjectDetectionSsdData
{
	unsigned int numDetections = 0;
	std::vector<PplBbox> vBbox;
	std::vector<float> vScores;
	std::vector<uint8_t> vClasses;
};

class MobileNet : public PostProcessingStage
{
public:
	MobileNet(RPiCamApp *app) : PostProcessingStage(app) {}

	char const *Name() const override;

	void Read(boost::property_tree::ptree const &params) override;

	void Configure() override;

	bool Process(CompletedRequestPtr &completed_request) override;

private:
	int parseHeader(const uint8_t *src, uint32_t stride);
	int parseApParams();
	int populateOutputBodyInfo();
	int parseOutputTensorBody(const uint8_t *src, uint32_t stride);
	ObjectDetectionSsdData processOutputTensor();

	Stream *stream_;
	DnnHeader dnnHeader_;
	std::vector<uint8_t> apParams_;
	std::string networkType_;
	std::vector<OutputTensorApParams> outputApParams_;
	OutputTensorInfo outputBodyInfo_;

	// Config params
	unsigned int maxDetections_;
	float threshold_;
	std::vector<std::string> classes_;
};

#define NAME "imx500_mobilenet"

char const *MobileNet::Name() const
{
	return NAME;
}

void MobileNet::Read(boost::property_tree::ptree const &params)
{
	maxDetections_ = params.get<unsigned int>("max_detections");
	threshold_ = params.get<float>("threshold", 0.3f);

	std::string classFile = params.get<std::string>("class_file");
	std::ifstream f(classFile);

	std::string c;
	while (std::getline(f, c))
		classes_.push_back(c);
}

void MobileNet::Configure()
{
	stream_ = app_->GetMainStream();
}

bool MobileNet::Process(CompletedRequestPtr &completed_request)
{
	auto output = completed_request->metadata.get(controls::rpi::Imx500OutputTensor);
	if (!output)
	{
		LOG_ERROR("No output tensor found in metadata!");
		return false;
	}

	const uint8_t *src = output->data();
	uint32_t stride = 4064; //(((stream_->configuration().size.width * 10) >> 3) + 15) & ~15;
	int ret = parseHeader(src, stride);
	if (ret)
	{
		LOG_ERROR("Header param parsing failed!");
		return false;
	}

	ret = parseApParams();
	if (ret)
	{
		LOG_ERROR("AP param parsing failed!");
		return false;
	}

	ret = populateOutputBodyInfo();
	if (ret)
	{
		LOG_ERROR("Failed to populate OutputBodyInfo!");
		return false;
	}

	ret = parseOutputTensorBody(src + stride, stride);
	if (ret)
	{
		LOG_ERROR("Output tensor body parsing failed!");
		return false;
	}

	ObjectDetectionSsdData data = processOutputTensor();
	if (data.numDetections)
	{
		std::vector<Detection> detections;

		for (unsigned int i = 0; i < data.numDetections; i++)
		{
			assert(data.vClasses[i] < classes_.size());
			detections.emplace_back(data.vClasses[i], classes_[data.vClasses[i]], data.vScores[i], data.vBbox[i].mXmin,
									data.vBbox[i].mYmin, data.vBbox[i].mXmax - data.vBbox[i].mXmin,
									data.vBbox[i].mYmax - data.vBbox[i].mYmin);
		}

		completed_request->post_process_metadata.Set("object_detect.results", detections);
	}

	return false;
}

int MobileNet::parseHeader(const uint8_t *src, uint32_t stride)
{
	constexpr unsigned int DnnHeaderSize = 12;
	constexpr unsigned int MipiPhSize = 0;

	dnnHeader_ = *(DnnHeader *)src;

	LOG(2, "Header: valid " << (bool)dnnHeader_.frameValid << " count " << (int)dnnHeader_.frameCount << " max len "
							<< dnnHeader_.maxLineLen << " ap param size " << dnnHeader_.apParamSize << " network id "
							<< dnnHeader_.networkId << " tensor type " << (int)dnnHeader_.tensorType);

	if (!dnnHeader_.frameValid)
		return -1;

	apParams_.resize(dnnHeader_.apParamSize, 0);

	uint32_t i = DnnHeaderSize;
	for (unsigned int j = 0; j < dnnHeader_.apParamSize; j++)
	{
		if (stride && i >= stride)
		{
			i = 0;
			src += stride + MipiPhSize;
		}
		apParams_[j] = src[i++];
	}

	return 0;
}

int MobileNet::parseApParams()
{
	const apParams::fb::FBApParams *fbApParams;
	const apParams::fb::FBNetwork *fbNetwork;
	const apParams::fb::FBOutputTensor *fbOutputTensor;

	fbApParams = apParams::fb::GetFBApParams(apParams_.data());
	LOG(2, "Networks size: " << fbApParams->networks()->size());

	outputApParams_.clear();

	for (unsigned int i = 0; i < fbApParams->networks()->size(); i++)
	{
		fbNetwork = (apParams::fb::FBNetwork *)(fbApParams->networks()->Get(i));
		if (fbNetwork->id() != dnnHeader_.networkId)
			continue;

		networkType_ = fbNetwork->type()->c_str();
		LOG(2, "Network: " << networkType_ << ", i/p size: " << fbNetwork->inputTensors()->size()
						   << ", o/p size: " << fbNetwork->outputTensors()->size());

		for (unsigned int j = 0; j < fbNetwork->outputTensors()->size(); j++)
		{
			OutputTensorApParams outApParam;

			fbOutputTensor = (apParams::fb::FBOutputTensor *)fbNetwork->outputTensors()->Get(j);

			outApParam.id = fbOutputTensor->id();
			outApParam.name = (char *)fbOutputTensor->name()->c_str();
			outApParam.numDimensions = fbOutputTensor->numOfDimensions();

			for (unsigned int k = 0; k < fbOutputTensor->numOfDimensions(); k++)
			{
				Dimensions dim;
				dim.ordinal = fbOutputTensor->dimensions()->Get(k)->id();
				dim.size = fbOutputTensor->dimensions()->Get(k)->size();
				dim.serializationIndex = fbOutputTensor->dimensions()->Get(k)->serializationIndex();
				dim.padding = fbOutputTensor->dimensions()->Get(k)->padding();
				if (dim.padding != 0)
				{
					LOG_ERROR("Error in AP Params, Non-Zero padding for Dimension " << k);
					return -1;
				}

				outApParam.vecDim.push_back(dim);
			}

			outApParam.bitsPerElement = fbOutputTensor->bitsPerElement();
			outApParam.shift = fbOutputTensor->shift();
			outApParam.scale = fbOutputTensor->scale();
			outApParam.format = fbOutputTensor->format();

			/* Add the element to vector */
			outputApParams_.push_back(outApParam);
		}

		break;
	}

	return 0;
}

int MobileNet::populateOutputBodyInfo()
{
	// Calculate total output size
	unsigned int totalOutSize = 0;
	for (auto const &ap : outputApParams_)
	{
		unsigned int totalDimensionSize = 1;
		for (auto &dim : ap.vecDim)
		{
			if (totalDimensionSize >= UINT32_MAX / dim.size)
			{
				LOG_ERROR("Invalid totalDimensionSize");
				return -1;
			}

			totalDimensionSize *= dim.size;
		}

		if (totalOutSize >= UINT32_MAX - totalDimensionSize)
		{
			LOG_ERROR("Invalid totalOutSize");
			return -1;
		}

		totalOutSize += totalDimensionSize;
	}

	// CodeSonar check
	if (totalOutSize == 0)
	{
		LOG_ERROR("Invalid output tensor info (totalOutSize is 0)");
		return -1;
	}

	LOG(2, "Final output size: " << totalOutSize);

	if (totalOutSize >= UINT32_MAX / sizeof(float))
	{
		LOG_ERROR("Invalid output tensor info");
		return -1;
	}

	// Set FrameOutputTensorInfo
	outputBodyInfo_ = {};
	outputBodyInfo_.address.resize(totalOutSize, 0.0f);
	unsigned int numOutputTensors = outputApParams_.size();

	/* CodeSonar Check */
	if (!numOutputTensors)
	{
		LOG_ERROR("Invalid numOutputTensors (0)");
		return -1;
	}

	if (numOutputTensors >= UINT32_MAX / sizeof(uint32_t))
	{
		LOG_ERROR("Invalid numOutputTensors");
		return -1;
	}

	outputBodyInfo_.totalSize = totalOutSize;
	outputBodyInfo_.tensorNum = numOutputTensors;
	outputBodyInfo_.tensorDataNum.resize(numOutputTensors, 0);

	return 0;
}

template <typename T>
float getVal8(const uint8_t *src, const OutputTensorApParams &param)
{
	T temp = (T)*src;
	float value = (temp - param.shift) * param.scale;
	return value;
}

#define bytes_to_uint16(MSB, LSB) (((uint16_t)((unsigned char)MSB)) & 255) << 8 | (((unsigned char)LSB) & 255)
#define bytes_to_int16(MSB, LSB) (((int16_t)((unsigned char)MSB)) & 255) << 8 | (((unsigned char)LSB) & 255)

int MobileNet::parseOutputTensorBody(const uint8_t *src, uint32_t stride)
{
	float *dst = outputBodyInfo_.address.data();
	int ret = 0;

	if (outputBodyInfo_.totalSize > (UINT32_MAX / sizeof(float)))
	{
		LOG_ERROR("totalSize is greater than maximum size");
		return -1;
	}

	std::vector<float> tmpDst(outputBodyInfo_.totalSize, 0.0f);
	std::vector<uint16_t> numLinesVec(outputApParams_.size());
	std::vector<uint32_t> outSizes(outputApParams_.size());
	std::vector<uint32_t> offsets(outputApParams_.size());
	std::vector<const uint8_t *> srcArr(outputApParams_.size());
	std::vector<std::vector<Dimensions>> serializedDims;
	std::vector<std::vector<Dimensions>> actualDims;

	const uint8_t *src1 = src;
	uint32_t offset = 0;
	for (unsigned int tensorIdx = 0; tensorIdx < outputApParams_.size(); tensorIdx++)
	{
		offsets[tensorIdx] = offset;
		srcArr[tensorIdx] = src1;
		uint32_t tensorDataNum = 0;

		const OutputTensorApParams &param = outputApParams_.at(tensorIdx);
		uint32_t outputTensorSize = 0;
		uint32_t tensorOutSize = (param.bitsPerElement / 8);
		std::vector<Dimensions> serializedDim(param.numDimensions);
		std::vector<Dimensions> actualDim(param.numDimensions);

		for (int idx = 0; idx < param.numDimensions; idx++)
		{
			actualDim[idx].size = param.vecDim.at(idx).size;
			serializedDim[param.vecDim.at(idx).serializationIndex].size = param.vecDim.at(idx).size;

			tensorOutSize *= param.vecDim.at(idx).size;
			if (tensorOutSize >= UINT32_MAX / param.bitsPerElement / 8)
			{
				LOG_ERROR("Invalid output tensor info");
				return -1;
			}

			actualDim[idx].serializationIndex = param.vecDim.at(idx).serializationIndex;
			serializedDim[param.vecDim.at(idx).serializationIndex].serializationIndex = (uint8_t)idx;
		}

		uint16_t numLines = (uint16_t)std::ceil(tensorOutSize / (float)dnnHeader_.maxLineLen);
		outputTensorSize = tensorOutSize;
		numLinesVec[tensorIdx] = numLines;
		outSizes[tensorIdx] = tensorOutSize;

		serializedDims.push_back(serializedDim);
		actualDims.push_back(actualDim);

		src1 += numLines * stride;
		tensorDataNum = (outputTensorSize / (param.bitsPerElement / 8));
		offset += tensorDataNum;
		outputBodyInfo_.tensorDataNum[tensorIdx] = tensorDataNum;
		if (offset > outputBodyInfo_.totalSize)
		{
			LOG_ERROR("Error in parsing output tensor offset " << offset << " > output_size");
			return -1;
		}
	}

	std::vector<uint32_t> idxs(outputApParams_.size());
	for (unsigned int i = 0; i < idxs.size(); i++)
		idxs[i] = i;

	for (unsigned int i = 0; i < idxs.size(); i++)
	{
		for (unsigned int j = 0; j < idxs.size(); j++)
		{
			if (numLinesVec[idxs[i]] > numLinesVec[idxs[j]])
				std::swap(idxs[i], idxs[j]);
		}
	}

	std::vector<std::future<int>> futures;
	for (unsigned int i = 0; i < idxs.size(); i++)
	{
		uint32_t tensorIdx = idxs[i];
		futures.emplace_back(std::async(
			[&tmpDst, &outSizes, &numLinesVec, &actualDims, &serializedDims, stride, dst, this]
			(int tensorIdx, const uint8_t *src, int offset) -> int
			{
				uint32_t outputTensorSize = outSizes[tensorIdx];
				uint16_t numLines = numLinesVec[tensorIdx];
				bool sortingRequired = false;

				const OutputTensorApParams &param = this->outputApParams_[tensorIdx];
				const std::vector<Dimensions> &serializedDim = serializedDims[tensorIdx];
				const std::vector<Dimensions> &actualDim = actualDims[tensorIdx];

				for (int idx = 0; idx < param.numDimensions; idx++)
				{
					if (param.vecDim.at(idx).serializationIndex != param.vecDim.at(idx).ordinal)
						sortingRequired = true;
				}

				if (!outputTensorSize)
				{
					LOG_ERROR("Invalid output tensorsize (0)");
					return -1;
				}

				// Extract output tensor data
				uint32_t elementIndex = 0;
				if (param.bitsPerElement == 8)
				{
					for (unsigned int i = 0; i < numLines; i++)
					{
						int lineIndex = 0;
						while (lineIndex < this->dnnHeader_.maxLineLen)
						{
							if (param.format == TYPE_SIGNED)
								tmpDst[offset + elementIndex] = getVal8<int8_t>(src + lineIndex, param);
							else
								tmpDst[offset + elementIndex] = getVal8<uint8_t>(src + lineIndex, param);
							elementIndex++;
							lineIndex++;
							if (elementIndex == outputTensorSize)
								break;
						}
						src += stride;
						if (elementIndex == outputTensorSize)
							break;
					}
				}
				else if (param.bitsPerElement == 16)
				{
					for (int i = 0; i < numLines; i++)
					{
						int lineIndex = 0;
						while (lineIndex < this->dnnHeader_.maxLineLen)
						{
							if (param.format == TYPE_SIGNED)
							{
								int16_t temp =
									bytes_to_int16((int8_t) * (src + lineIndex + 1), (int8_t) * (src + lineIndex));
								float value = (temp - param.shift) * param.scale;
								tmpDst[offset + elementIndex] = value;
							}
							else
							{
								uint16_t temp =
									bytes_to_uint16((uint8_t) * (src + lineIndex + 1), (uint8_t) * (src + lineIndex));
								float value = (temp - param.shift) * param.scale;
								tmpDst[offset + elementIndex] = value;
							}
							elementIndex++;
							lineIndex += 2;
							if (elementIndex >= (outputTensorSize >> 1))
								break;
						}
						src += stride;
						if (elementIndex >= (outputTensorSize >> 1))
							break;
					}
				}
				else
				{
					LOG_ERROR("Invalid bitsPerElement value =" << param.bitsPerElement);
					return -1;
				}

				// Sorting in order according to AP Params. Not supported if larger than 3D
				// Preparation
				if (sortingRequired)
				{
					constexpr unsigned int DimensionMax = 3;

					std::array<uint32_t, DimensionMax> loopCnt { 1, 1, 1 };
					std::array<uint32_t, DimensionMax> coef { 1, 1, 1 };
					for (unsigned int i = 0; i < param.numDimensions; i++)
					{
						if (i >= DimensionMax)
						{
							LOG_ERROR("numDimensions value is 3 or higher");
							break;
						}

						loopCnt[i] = serializedDim.at(i).size;

						for (unsigned int j = serializedDim.at(i).serializationIndex; j > 0; j--)
							coef[i] *= actualDim.at(j - 1).size;
					}
					// Sort execution
					unsigned int src_index = 0;
					unsigned int dst_index;
					for (unsigned int i = 0; i < loopCnt[DimensionMax - 1]; i++)
					{
						for (unsigned int j = 0; j < loopCnt[DimensionMax - 2]; j++)
						{
							for (unsigned int k = 0; k < loopCnt[DimensionMax - 3]; k++)
							{
								dst_index = (coef[DimensionMax - 1] * i) + (coef[DimensionMax - 2] * j) +
											(coef[DimensionMax - 3] * k);
								dst[offset + dst_index] = tmpDst[offset + src_index++];
							}
						}
					}
				}
				else
				{
					if (param.bitsPerElement == 8)
						memcpy(dst + offset, tmpDst.data() + offset, outputTensorSize * sizeof(float));
					else if (param.bitsPerElement == 16)
						memcpy(dst + offset, tmpDst.data() + offset, (outputTensorSize >> 1) * sizeof(float));
					else
					{
						LOG_ERROR("Invalid bitsPerElement value =" << param.bitsPerElement);
						return -1;
					}
				}

				return 0;
			},
			tensorIdx, srcArr[tensorIdx], offsets[tensorIdx]));
	}

	for (auto &f : futures)
		ret += f.get();

	return ret;
}

static int createObjectDetectionSsdData(ObjectDetectionSsdOutputTensor &output, const std::vector<float> &data,
										unsigned int totalDetections)
{
	unsigned int count = 0;

	if ((count + (totalDetections * 4)) > PplDnnOutputTensorSize)
	{
		LOG_ERROR("Invalid count index " << count);
		return -1;
	}

	// Extract bounding box co-ordinates
	for (unsigned int i = 0; i < totalDetections; i++)
	{
		Bbox bbox;
		bbox.yMin = data.at(count + i);
		bbox.xMin = data.at(count + i + (1 * totalDetections));
		bbox.yMax = data.at(count + i + (2 * totalDetections));
		bbox.xMax = data.at(count + i + (3 * totalDetections));
		output.bboxes.push_back(bbox);
	}
	count += (totalDetections * 4);

	// Extract class indices
	for (unsigned int i = 0; i < totalDetections; i++)
	{
		if (count > PplDnnOutputTensorSize)
		{
			LOG_ERROR("Invalid count index " << count);
			return -1;
		}

		output.classes.push_back(data.at(count));
		count++;
	}

	// Extract scores
	for (unsigned int i = 0; i < totalDetections; i++)
	{
		if (count > PplDnnOutputTensorSize)
		{
			LOG_ERROR("Invalid count index " << count);
			return -1;
		}

		output.scores.push_back(data.at(count));
		count++;
	}

	if (count > PplDnnOutputTensorSize)
	{
		LOG_ERROR("Invalid count index " << count);
		return -1;
	}

	// Extract number of detections
	unsigned int numDetections = data.at(count);
	if (numDetections > totalDetections)
	{
		LOG(1, "Unexpected value for numDetections: " << numDetections << ", setting it to " << totalDetections);
		numDetections = totalDetections;
	}

	output.numDetections = numDetections;
	return 0;
}

static ObjectDetectionSsdData analyseObjectDetectionSsdOutput(const ObjectDetectionSsdOutputTensor &tensor,
															  unsigned int maxDetections, float threshold,
															  libcamera::Size dim)
{
	ObjectDetectionSsdData data;

	for (unsigned int i = 0; i < tensor.numDetections; i++)
	{
		// Filter detections
		if (tensor.scores[i] < threshold)
			continue;

		data.vScores.push_back(tensor.scores[i]);

		// Extract bounding box co-ordinates
		PplBbox bbox;
		bbox.mXmin = (uint16_t)(round((tensor.bboxes[i].xMin) * (dim.width - 1)));
		bbox.mYmin = (uint16_t)(round((tensor.bboxes[i].yMin) * (dim.height - 1)));
		bbox.mXmax = (uint16_t)(round((tensor.bboxes[i].xMax) * (dim.width - 1)));
		bbox.mYmax = (uint16_t)(round((tensor.bboxes[i].yMax) * (dim.height - 1)));
		data.vBbox.push_back(bbox);

		// Extract classes
		data.vClasses.push_back((uint8_t)tensor.classes[i]);
	}

	data.numDetections = data.vClasses.size();

	if (data.numDetections > maxDetections)
	{
		data.numDetections = maxDetections;
		data.vBbox.resize(maxDetections);
		data.vClasses.resize(maxDetections);
		data.vScores.resize(maxDetections);
	}

	LOG(2, "Number of detections: " << data.numDetections);
	for (unsigned i = 0; i < data.numDetections; i++)
	{
		LOG(2, "[" << i << "] = ["
			   << data.vBbox[i].mXmin << ", "
			   << data.vBbox[i].mXmax << ", "
			   << data.vBbox[i].mYmin << ", "
			   << data.vBbox[i].mYmax << "]"
			   << ", score " << data.vScores[i]
			   << ", class " << (int)data.vClasses[i]);
	}

	return data;
}

ObjectDetectionSsdData MobileNet::processOutputTensor()
{
	ObjectDetectionSsdOutputTensor output;
	ObjectDetectionSsdData data;

	if (outputBodyInfo_.totalSize != PplDnnOutputTensorSize)
	{
		LOG_ERROR("Invalid totalSize " << outputBodyInfo_.totalSize);
		return {};
	}

	int ret = createObjectDetectionSsdData(output, outputBodyInfo_.address, PplTotalDetections);
	if (ret)
	{
		LOG_ERROR("Failed to create SSD data");
		return {};
	}

	libcamera::Size dim = stream_->configuration().size;
	return analyseObjectDetectionSsdOutput(output, maxDetections_, threshold_, dim);
}

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new MobileNet(app);
}

static RegisterStage reg(NAME, &Create);
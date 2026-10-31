#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <aacdecoder_lib.h>
#include <aacenc_lib.h>

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kSamplesPerFrame = 480;
constexpr int kFrameCount = 20;

bool set_encoder_parameter(HANDLE_AACENCODER encoder, AACENC_PARAM parameter, UINT value,
			   const char *name)
{
	const AACENC_ERROR error = aacEncoder_SetParam(encoder, parameter, value);
	if (error == AACENC_OK)
		return true;
	std::fprintf(stderr, "Set %s failed: FDK error %d\n", name,
		     static_cast<int>(error));
	return false;
}

} // namespace

int main()
{
	HANDLE_AACENCODER encoder = nullptr;
	HANDLE_AACDECODER decoder = nullptr;
	if (aacEncOpen(&encoder, 0, kChannels) != AACENC_OK) {
		std::fprintf(stderr, "Unable to open FDK encoder\n");
		return 1;
	}
	const bool configured =
		set_encoder_parameter(encoder, AACENC_AOT, AOT_ER_AAC_ELD, "AOT") &&
		set_encoder_parameter(encoder, AACENC_SAMPLERATE, kSampleRate, "sample rate") &&
		set_encoder_parameter(encoder, AACENC_CHANNELMODE, MODE_2, "channel mode") &&
		set_encoder_parameter(encoder, AACENC_CHANNELORDER, 1, "channel order") &&
		set_encoder_parameter(encoder, AACENC_BITRATE, 256000, "bitrate") &&
		set_encoder_parameter(encoder, AACENC_TRANSMUX, TT_MP4_RAW, "transmux") &&
		set_encoder_parameter(encoder, AACENC_SBR_MODE, 0, "SBR mode") &&
		set_encoder_parameter(encoder, AACENC_GRANULE_LENGTH, kSamplesPerFrame,
				      "granule length") &&
		set_encoder_parameter(encoder, AACENC_AFTERBURNER, 1, "afterburner");
	if (!configured ||
	    aacEncEncode(encoder, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
		aacEncClose(&encoder);
		return 1;
	}

	AACENC_InfoStruct encoder_info = {};
	if (aacEncInfo(encoder, &encoder_info) != AACENC_OK ||
	    encoder_info.frameLength != kSamplesPerFrame) {
		std::fprintf(stderr, "Unexpected encoder frame length %u\n",
			     encoder_info.frameLength);
		aacEncClose(&encoder);
		return 1;
	}

	decoder = aacDecoder_Open(TT_MP4_RAW, 1);
	if (decoder == nullptr) {
		std::fprintf(stderr, "Unable to open FDK decoder\n");
		aacEncClose(&encoder);
		return 1;
	}
	UCHAR *config[] = {encoder_info.confBuf};
	if (aacDecoder_ConfigRaw(decoder, config, &encoder_info.confSize) != AAC_DEC_OK) {
		std::fprintf(stderr, "Unable to configure FDK decoder\n");
		aacDecoder_Close(decoder);
		aacEncClose(&encoder);
		return 1;
	}

	std::array<INT_PCM, kSamplesPerFrame * kChannels> pcm = {};
	std::array<std::uint8_t, 8192> encoded = {};
	std::array<INT_PCM, 8192> decoded = {};
	std::int64_t squared_sum = 0;
	std::uint64_t decoded_samples = 0;
	int decoded_peak = 0;
	int encoded_bytes = 0;

	for (int frame = 0; frame < kFrameCount; frame++) {
		for (int sample = 0; sample < kSamplesPerFrame; sample++) {
			const double phase = 2.0 * 3.14159265358979323846 * 1000.0 *
					     (frame * kSamplesPerFrame + sample) / kSampleRate;
			const auto value = static_cast<INT_PCM>(
				std::lround(std::sin(phase) * 0.5 * 32767.0));
			pcm[sample * 2] = value;
			pcm[sample * 2 + 1] = value;
		}

		AACENC_BufDesc input_description = {};
		AACENC_BufDesc output_description = {};
		AACENC_InArgs input_arguments = {};
		AACENC_OutArgs output_arguments = {};
		void *input_buffers[] = {pcm.data()};
		void *output_buffers[] = {encoded.data()};
		INT input_identifiers[] = {IN_AUDIO_DATA};
		INT output_identifiers[] = {OUT_BITSTREAM_DATA};
		INT input_sizes[] = {sizeof(pcm)};
		INT output_sizes[] = {static_cast<INT>(encoded.size())};
		INT input_element_sizes[] = {sizeof(INT_PCM)};
		INT output_element_sizes[] = {1};

		input_description.numBufs = 1;
		input_description.bufs = input_buffers;
		input_description.bufferIdentifiers = input_identifiers;
		input_description.bufSizes = input_sizes;
		input_description.bufElSizes = input_element_sizes;
		output_description.numBufs = 1;
		output_description.bufs = output_buffers;
		output_description.bufferIdentifiers = output_identifiers;
		output_description.bufSizes = output_sizes;
		output_description.bufElSizes = output_element_sizes;
		input_arguments.numInSamples = kSamplesPerFrame * kChannels;

		const AACENC_ERROR encode_error =
			aacEncEncode(encoder, &input_description, &output_description,
				     &input_arguments, &output_arguments);
		if (encode_error != AACENC_OK || output_arguments.numOutBytes <= 0) {
			std::fprintf(stderr, "Encode failed: %d, output %d\n",
				     static_cast<int>(encode_error),
				     output_arguments.numOutBytes);
			aacDecoder_Close(decoder);
			aacEncClose(&encoder);
			return 1;
		}
		encoded_bytes = output_arguments.numOutBytes;

		UCHAR *input = encoded.data();
		UINT input_size = static_cast<UINT>(encoded_bytes);
		UINT valid = input_size;
		const AAC_DECODER_ERROR fill_error =
			aacDecoder_Fill(decoder, &input, &input_size, &valid);
		if (fill_error != AAC_DEC_OK) {
			std::fprintf(stderr, "Decode fill failed: %d\n",
				     static_cast<int>(fill_error));
			aacDecoder_Close(decoder);
			aacEncClose(&encoder);
			return 1;
		}
		const AAC_DECODER_ERROR decode_error = aacDecoder_DecodeFrame(
			decoder, decoded.data(), static_cast<INT>(decoded.size()), 0);
		if (decode_error != AAC_DEC_OK) {
			std::fprintf(stderr, "Decode failed: %d\n",
				     static_cast<int>(decode_error));
			aacDecoder_Close(decoder);
			aacEncClose(&encoder);
			return 1;
		}
		const CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
		if (info == nullptr || info->frameSize != kSamplesPerFrame ||
		    info->numChannels != kChannels) {
			std::fprintf(stderr, "Unexpected decoder format\n");
			aacDecoder_Close(decoder);
			aacEncClose(&encoder);
			return 1;
		}
		const int output_samples = info->frameSize * info->numChannels;
		for (int index = 0; index < output_samples; index++) {
			const int sample = decoded[index];
			decoded_peak = std::max(decoded_peak, std::abs(sample));
			squared_sum += static_cast<std::int64_t>(sample) * sample;
		}
		decoded_samples += output_samples;
	}

	const double rms =
		std::sqrt(static_cast<double>(squared_sum) / decoded_samples);
	std::printf("encoded=%d bytes/frame decoded=%llu samples peak=%d rms=%.1f\n",
		    encoded_bytes, static_cast<unsigned long long>(decoded_samples),
		    decoded_peak, rms);

	aacDecoder_Close(decoder);
	aacEncClose(&encoder);
	return decoded_peak > 1000 ? 0 : 2;
}

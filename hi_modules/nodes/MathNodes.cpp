/*  ===========================================================================
*
*   This file is part of HISE.
*   Copyright 2016 Christoph Hart
*
*   HISE is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   HISE is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with HISE.  If not, see <http://www.gnu.org/licenses/>.
*
*   Commercial licenses for using HISE in an closed source project are
*   available on request. Please visit the project's website to get more
*   information about commercial licensing:
*
*   http://www.hise.audio/
*
*   HISE is based on the JUCE library,
*   which must be separately licensed for closed source applications:
*
*   http://www.juce.com
*
*   ===========================================================================
*/

namespace scriptnode {
using namespace juce;
using namespace hise;

namespace math
{


template <class OpType, int V>
bool scriptnode::math::OpNode<OpType, V>::handleModulation(double&) noexcept
{
	return false;
}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::process(ProcessData& d)
{
	OpType::op(d, value.get());
}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::processSingle(float* frameData, int numChannels)
{
	OpType::opSingle(frameData, numChannels, value.get());
}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::reset() noexcept
{

}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::prepare(PrepareSpecs ps)
{
	value.prepare(ps);
}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::createParameters(Array<ParameterData>& data)
{
	ParameterData p("Value");
	p.range = { 0.0, 1.0, 0.01 };
	p.defaultValue = OpType::defaultValue;

	p.db = BIND_MEMBER_FUNCTION_1(OpNode::setValue);

	data.add(std::move(p));
}

template <class OpType, int V>
void scriptnode::math::OpNode<OpType, V>::setValue(double newValue)
{
	if (NumVoices == 1)
	{
		value.getMonoValue() = (float)newValue;
	}
	else
	{
		if (value.isVoiceRenderingActive())
		{
			value.get() = (float)newValue;
		}
		else
		{
			auto nv = (float)newValue;
			value.forEachVoice([nv](float& v)
			{
				v = nv;
			});
		}
	}
}

DEFINE_OP_NODE_IMPL(mul);
DEFINE_OP_NODE_IMPL(add);
DEFINE_OP_NODE_IMPL(sub);
DEFINE_OP_NODE_IMPL(div);
DEFINE_OP_NODE_IMPL(tanh);
DEFINE_OP_NODE_IMPL(clip);
DEFINE_OP_NODE_IMPL(sin);
DEFINE_OP_NODE_IMPL(pi);
DEFINE_OP_NODE_IMPL(sig2mod);
DEFINE_OP_NODE_IMPL(abs);
DEFINE_OP_NODE_IMPL(clear);

Factory::Factory(DspNetwork* n) :
	NodeFactory(n)
{
	registerPolyNode<mul, mul_poly>();
	registerPolyNode<add, add_poly>();
	registerNode<clear>();
	registerPolyNode<sub, sub_poly>();
	registerPolyNode<div, div_poly>();
	registerPolyNode<tanh, tanh_poly>();
	registerPolyNode<clip, clip_poly>();
	registerNode<sin>();
	registerPolyNode<pi, pi_poly>();
	registerNode<sig2mod>();
	registerPolyNode<abs, abs_poly>();
}


void Operations::mul::op(ProcessData& d, float value)
{
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::multiply(d.data[i], value, d.size);
}

void Operations::mul::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ *= value;
}

void Operations::add::op(ProcessData& d, float value)
{
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::add(d.data[i], value, d.size);
}

void Operations::add::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ += value;
}

void Operations::clear::op(ProcessData& d, float)
{
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::clear(d.data[i], d.size);
}

void Operations::clear::opSingle(float* frameData, int numChannels, float)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ = 0.0f;
}

void Operations::sub::op(ProcessData& d, float value)
{
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::add(d.data[i], -value, d.size);
}

void Operations::sub::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ -= value;
}

void Operations::div::op(ProcessData& d, float value)
{
	auto factor = value > 0.0f ? 1.0f / value : 0.0f;
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::multiply(d.data[i], factor, d.size);
}

void Operations::div::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ /= value;
}

void Operations::tanh::op(ProcessData& d, float value)
{
	for (int i = 0; i < d.numChannels; i++)
	{
		auto ptr = d.data[i];

		for (int j = 0; j < d.size; j++)
			ptr[j] = std::tanh(ptr[j] * value);
	}
}

void Operations::tanh::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		frameData[i] = std::tanh(frameData[i] * value);
}

void Operations::pi::op(ProcessData& d, float value)
{
	for (auto ptr : d)
		FloatVectorOperations::multiply(ptr, float_Pi * value, d.size);
}

void Operations::pi::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		*frameData++ *= float_Pi * value;
}

void Operations::sin::op(ProcessData& d, float)
{
	for (auto ptr : d)
	{
		for (int i = 0; i < d.size; i++)
			ptr[i] = std::sin(ptr[i]);
	}
}

void Operations::sin::opSingle(float* frameData, int numChannels, float)
{
	for (int i = 0; i < numChannels; i++)
		frameData[i] = std::sin(frameData[i]);
}

void Operations::sig2mod::op(ProcessData& d, float)
{
	for (auto& ch : d.channels())
		for (auto& s : ch)
			s = s * 0.5f + 0.5f;
}

void Operations::sig2mod::opSingle(float* frameData, int numChannels, float)
{
	for (int i = 0; i < numChannels; i++)
		frameData[i] = frameData[i] * 0.5f + 0.5f;
}

void Operations::clip::op(ProcessData& d, float value)
{
	for (auto ptr : d)
		FloatVectorOperations::clip(ptr, ptr, -value, value, d.size);
}

void Operations::clip::opSingle(float* frameData, int numChannels, float value)
{
	for (int i = 0; i < numChannels; i++)
		frameData[i] = jlimit(-value, value, frameData[i]);
}

void Operations::abs::op(ProcessData& d, float)
{
	for (int i = 0; i < d.numChannels; i++)
		FloatVectorOperations::abs(d.data[i], d.data[i], d.size);
}

void Operations::abs::opSingle(float* frameData, int numChannels, float)
{
	for (int i = 0; i < numChannels; i++)
		frameData[i] = frameData[i] > 0.0f ? frameData[i] : frameData[i] * -1.0f;
}

}

}
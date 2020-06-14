#include <SHADERed/Objects/SPIRVParser.h>
#include <spirv/spirv.hpp>
#include <unordered_map>
#include <functional>

typedef unsigned int spv_word;

namespace ed {
	std::string spvReadString(const unsigned int* data, int length, int& i)
	{
		std::string ret(length * 4, 0);

		for (int j = 0; j < length; j++, i++) {
			ret[j*4+0] = (data[i] & 0x000000FF);
			ret[j*4+1] = (data[i] & 0x0000FF00) >> 8;
			ret[j*4+2] = (data[i] & 0x00FF0000) >> 16;
			ret[j*4+3] = (data[i] & 0xFF000000) >> 24;
		}

		return ret;
	}

	void SPIRVParser::Parse(const std::vector<unsigned int>& ir)
	{
		Functions.clear();
		UserTypes.clear();
		Uniforms.clear();
		Globals.clear();

		std::string curFunc = "";
		int lastOpLine = -1;

		std::unordered_map<spv_word, std::string> names;
		std::unordered_map<spv_word, spv_word> pointers;
		std::unordered_map<spv_word, std::pair<ValueType, int>> types;

		std::function<void(Variable&, spv_word)> fetchType = [&](Variable& var, spv_word type) {
			spv_word actualType = type;
			if (pointers.count(type))
				actualType = pointers[type];

			const std::pair<ValueType, int>& info = types[actualType];
			var.Type = info.first;
			
			if (var.Type == ValueType::Struct)
				var.TypeName = names[info.second];
			else if (var.Type == ValueType::Vector || var.Type == ValueType::Matrix) {
				var.TypeComponentCount = info.second & 0x00ffffff;
				var.BaseType = (ValueType)((info.second & 0xff000000) >> 24);
			} 
		};

		for (int i = 5; i < ir.size();) {
			int iStart = i;
			spv_word opcodeData = ir[i];

			spv_word wordCount = ((opcodeData & (~spv::OpCodeMask)) >> spv::WordCountShift) - 1;
			spv_word opcode = (opcodeData & spv::OpCodeMask);

			if (opcode == spv::OpName) {
				spv_word loc = ir[++i];
				spv_word stringLength = wordCount - 1;

				names[loc] = spvReadString(ir.data(), stringLength, ++i);
			} else if (opcode == spv::OpLine) {
				++i; // skip file
				lastOpLine = ir[++i];

				if (!curFunc.empty() && Functions[curFunc].LineStart == -1)
					Functions[curFunc].LineStart = lastOpLine;
			} else if (opcode == spv::OpTypeStruct) {
				spv_word loc = ir[++i];

				spv_word memCount = wordCount - 1;
				if (UserTypes.count(names[loc]) == 0) {
					std::vector<Variable> mems(memCount);
					for (spv_word j = 0; j < memCount; j++) {
						spv_word type = ir[++i];
						fetchType(mems[j], type);
					}

					UserTypes.insert(std::make_pair(names[loc], mems));
				} else {
					auto& typeInfo = UserTypes[names[loc]];
					for (spv_word j = 0; j < memCount && j < typeInfo.size(); j++) {
						spv_word type = ir[++i];
						fetchType(typeInfo[j], type);
					}
				}

				types[loc] = std::make_pair(ValueType::Struct, loc);
			} else if (opcode == spv::OpMemberName) {
				spv_word owner = ir[++i];
				spv_word index = ir[++i]; // index

				spv_word stringLength = wordCount - 2;

				auto& typeInfo = UserTypes[names[owner]];

				if (index < typeInfo.size())
					typeInfo[index].Name = spvReadString(ir.data(), stringLength, ++i);
				else {
					typeInfo.resize(index + 1);
					typeInfo[index].Name = spvReadString(ir.data(), stringLength, ++i);
				}
			} else if (opcode == spv::OpFunction) {
				++i; // skip type
				spv_word loc = ir[++i];

				curFunc = names[loc];
				size_t args = curFunc.find_first_of('(');
				if (args != std::string::npos)
					curFunc = curFunc.substr(0, args);

				Functions[curFunc].LineStart = -1;
			} else if (opcode == spv::OpFunctionEnd) {
				Functions[curFunc].LineEnd = lastOpLine;
				lastOpLine = -1;
				curFunc = "";
			} else if (opcode == spv::OpVariable) {
				spv_word type = ir[++i]; // skip type
				spv_word loc = ir[++i];

				std::string varName = names[loc];

				if (curFunc.empty()) {
					spv::StorageClass sType = (spv::StorageClass)ir[++i];
					if (sType == spv::StorageClassUniform || sType == spv::StorageClassUniformConstant) {
						Variable uni;
						uni.Name = varName;
						fetchType(uni, type);

						if (uni.Name.size() == 0 || uni.Name[0] == 0) {
							if (UserTypes.count(uni.TypeName) > 0) {
								const std::vector<Variable>& mems = UserTypes[uni.TypeName];
								for (const auto& mem : mems)
									Uniforms.push_back(mem);
							}
						} else 
							Uniforms.push_back(uni);
					} else if (varName.size() > 0 && varName[0] != 0)
						Globals.push_back(varName);
				} else
					Functions[curFunc].Locals.push_back(varName);
			} else if (opcode == spv::OpFunctionParameter) {
				++i; // skip type
				spv_word loc = ir[++i];

				std::string varName = names[loc];
				Functions[curFunc].Arguments.push_back(varName);
			} else if (opcode == spv::OpTypePointer) {
				spv_word loc = ir[++i];
				++i; // skip storage class
				spv_word type = ir[++i];

				pointers[loc] = type;
			} else if (opcode == spv::OpTypeBool) {
				spv_word loc = ir[++i];
				types[loc] = std::make_pair(ValueType::Bool, 0);
			} else if (opcode == spv::OpTypeInt) {
				spv_word loc = ir[++i];
				types[loc] = std::make_pair(ValueType::Int, 0);
			} else if (opcode == spv::OpTypeFloat) {
				spv_word loc = ir[++i];
				types[loc] = std::make_pair(ValueType::Float, 0);
			} else if (opcode == spv::OpTypeVector) {
				spv_word loc = ir[++i];
				spv_word comp = ir[++i];
				spv_word compcount = ir[++i];

				spv_word val = (compcount & 0x00FFFFFF) | (((spv_word)types[comp].first) << 24);

				types[loc] = std::make_pair(ValueType::Vector, val);
			} else if (opcode == spv::OpTypeMatrix) {
				spv_word loc = ir[++i];
				spv_word comp = ir[++i];
				spv_word compcount = ir[++i];

				spv_word val = (compcount & 0x00FFFFFF) | (types[comp].second & 0xFF000000);

				types[loc] = std::make_pair(ValueType::Matrix, val);
			}

			i = iStart + wordCount + 1;
		}
	}
}
#include "stdafx.h"
#include "TraceLogger.h"
#include "DisassemblyInfo.h"
#include "DebuggerTypes.h"
#include "Console.h"
#include "Debugger.h"
#include "MemoryManager.h"
#include "LabelManager.h"
#include "EmulationSettings.h"
#include "ExpressionEvaluator.h"
#include "../Utilities/HexUtilities.h"
#include "../Utilities/FolderUtilities.h"

TraceLogger *TraceLogger::_instance = nullptr;

TraceLogger::TraceLogger(Debugger* debugger, shared_ptr<MemoryManager> memoryManager, shared_ptr<LabelManager> labelManager)
{
	_expEvaluator = shared_ptr<ExpressionEvaluator>(new ExpressionEvaluator(debugger));
	_memoryManager = memoryManager;
	_labelManager = labelManager;
	_instance = this;
	_currentPos = 0;
	_logToFile = false;
}

TraceLogger::~TraceLogger()
{
	StopLogging();
	_instance = nullptr;
}

void TraceLogger::SetOptions(TraceLoggerOptions options)
{
	_options = options;
	string condition = _options.Condition;
	
	auto lock = _lock.AcquireSafe();
	_conditionRpnList.clear();
	if(!condition.empty()) {
		vector<int> *rpnList = _expEvaluator->GetRpnList(condition);
		if(rpnList) {
			_conditionRpnList = *rpnList;
		}
	}
}

void TraceLogger::StartLogging(string filename)
{
	_outputFile.open(filename, ios::out | ios::binary);
	_logToFile = true;
	_firstLine = true;
}

void TraceLogger::StopLogging() 
{
	if(_logToFile) {
		Console::Pause();
		if(_outputFile) {
			if(!_outputBuffer.empty()) {
				_outputFile << _outputBuffer;
			}
			_outputFile.close();
		}
		Console::Resume();
		_logToFile = false;
	}
}


void TraceLogger::LogStatic(string log)
{
	if(_instance && _instance->_logToFile && _instance->_options.ShowExtraInfo && !_instance->_firstLine) {
		//Flush current buffer
		_instance->_outputFile << _instance->_outputBuffer;
		_instance->_outputBuffer.clear();

		_instance->_outputFile << " - [" << log << " - Cycle: " << std::to_string(CPU::GetCycleCount()) << "]";
	}
}

void TraceLogger::GetStatusFlag(string &output, uint8_t ps)
{
	output += " P:";
	if(_options.StatusFormat == StatusFlagFormat::Hexadecimal) {
		output.append(HexUtilities::ToHex(ps));
	} else {
		constexpr char activeStatusLetters[8] = { 'N', 'V', 'B', '-', 'D', 'I', 'Z', 'C' };
		constexpr char inactiveStatusLetters[8] = { 'n', 'v', 'b', '-', 'd', 'i', 'z', 'c' };
		int padding = 6;
		for(int i = 0; i < 8; i++) {
			if(ps & 0x80) {
				output += activeStatusLetters[i];
				padding--;
			} else if(_options.StatusFormat == StatusFlagFormat::Text) {
				output += inactiveStatusLetters[i];
				padding--;
			}
			ps <<= 1;
		}
		if(padding > 0) {
			output += string(padding, ' ');
		}
	}
}

void TraceLogger::GetTraceRow(string &output, State &cpuState, PPUDebugState &ppuState, shared_ptr<DisassemblyInfo> &disassemblyInfo, bool firstLine)
{
	if(!firstLine) {
		output += "\n";
	}

	output += HexUtilities::ToHex(cpuState.DebugPC) + "  ";

	if(_options.ShowByteCode) {
		string byteCode;
		disassemblyInfo->GetByteCode(byteCode);
		output += byteCode + std::string(13 - byteCode.size(), ' ');
	}

	int indentLevel = 0;
	if(_options.IndentCode) {
		indentLevel = 0xFF - cpuState.SP;
		output += std::string(indentLevel, ' ');
	}

	string code;
	LabelManager* labelManager = _options.UseLabels ? _labelManager.get() : nullptr;
	disassemblyInfo->ToString(code, cpuState.DebugPC, _memoryManager.get(), labelManager);
	disassemblyInfo->GetEffectiveAddressString(code, cpuState, _memoryManager.get(), labelManager);
	code += std::string(std::max(0, (int)(32 - code.size())), ' ');
	output += code;

	if(_options.ShowRegisters) {
		output += " A:" + HexUtilities::ToHex(cpuState.A) +
			" X:" + HexUtilities::ToHex(cpuState.X) +
			" Y:" + HexUtilities::ToHex(cpuState.Y);

		GetStatusFlag(output, cpuState.PS);

		output += " SP:" + HexUtilities::ToHex(cpuState.SP);
	}

	if(_options.ShowPpuCycles) {
		string str = std::to_string(ppuState.Cycle);
		output += " CYC:" + std::string(3 - str.size(), ' ') + str;
	}

	if(_options.ShowPpuScanline) {
		string str = std::to_string(ppuState.Scanline);
		output += " SL:" + std::string(3 - str.size(), ' ') + str;
	}

	if(_options.ShowPpuFrames) {
		output += " FC:" + std::to_string(ppuState.FrameCount);
	}

	if(_options.ShowCpuCycles) {
		output += " CPU Cycle:" + std::to_string(cpuState.CycleCount);
	}
}

bool TraceLogger::ConditionMatches(DebugState &state, shared_ptr<DisassemblyInfo> &disassemblyInfo, OperationInfo &operationInfo)
{
	if(!_conditionRpnList.empty()) {
		EvalResultType type;
		if(!_expEvaluator->Evaluate(_conditionRpnList, state, type, operationInfo)) {
			if(operationInfo.OperationType == MemoryOperationType::ExecOpCode) {
				//Condition did not match, keep state/disassembly info for instruction's subsequent cycles
				_lastState = state;
				_lastDisassemblyInfo = disassemblyInfo;
			}
			return false;
		}
	}
	return true;
}

void TraceLogger::AddRow(shared_ptr<DisassemblyInfo> &disassemblyInfo, DebugState &state)
{
	_disassemblyCache[_currentPos] = disassemblyInfo;
	_cpuStateCache[_currentPos] = state.CPU;
	_ppuStateCache[_currentPos] = state.PPU;
	_currentPos = (_currentPos + 1) % ExecutionLogSize;
	_lastDisassemblyInfo.reset();

	if(_logToFile) {
		GetTraceRow(_outputBuffer, state.CPU, state.PPU, disassemblyInfo, _firstLine);
		if(_outputBuffer.size() > 32768) {
			_outputFile << _outputBuffer;
			_outputBuffer.clear();
		}

		_firstLine = false;
	}
}

void TraceLogger::LogNonExec(OperationInfo& operationInfo)
{
	if(_lastDisassemblyInfo) {
		auto lock = _lock.AcquireSafe();
		if(ConditionMatches(_lastState, _lastDisassemblyInfo, operationInfo)) {
			AddRow(_lastDisassemblyInfo, _lastState);
		}
	}
}

void TraceLogger::Log(DebugState &state, shared_ptr<DisassemblyInfo> disassemblyInfo, OperationInfo &operationInfo)
{
	if(disassemblyInfo) {
		auto lock = _lock.AcquireSafe();
		if(ConditionMatches(state, disassemblyInfo, operationInfo)) {
			AddRow(disassemblyInfo, state);
		}
	}
}

const char* TraceLogger::GetExecutionTrace(uint32_t lineCount)
{
	_executionTrace.clear();
	auto lock = _lock.AcquireSafe();
	int startPos = _currentPos + ExecutionLogSize - lineCount;
	bool firstLine = true;
	for(int i = 0; i < (int)lineCount; i++) {
		int index = (startPos + i) % ExecutionLogSize;
		if(_disassemblyCache[index]) {
			GetTraceRow(_executionTrace, _cpuStateCache[index], _ppuStateCache[index], _disassemblyCache[index], firstLine);
			firstLine = false;
		}
	}
	return _executionTrace.c_str();
}
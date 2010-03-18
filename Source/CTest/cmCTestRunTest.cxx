/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2000-2009 Kitware, Inc., Insight Software Consortium

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/

#include "cmCTestRunTest.h"
#include "cmCTestMemCheckHandler.h"
#include "cmCTest.h"
#include "cmSystemTools.h"
#include "cm_curl.h"

#include <cm_zlib.h>
#include <cmsys/Base64.h>

cmCTestRunTest::cmCTestRunTest(cmCTestTestHandler* handler)
{
  this->CTest = handler->CTest;
  this->TestHandler = handler;
  this->ModifyEnv = false;
  this->TestProcess = 0;
  this->TestResult.ExecutionTime =0;
  this->TestResult.ReturnValue = 0;
  this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
  this->TestResult.TestCount = 0;
  this->TestResult.Properties = 0;
  this->ProcessOutput = "";
  this->CompressedOutput = "";
  this->CompressionRatio = 2;
}

cmCTestRunTest::~cmCTestRunTest()
{
}

//----------------------------------------------------------------------------
bool cmCTestRunTest::CheckOutput()
{
  // Read lines for up to 0.1 seconds of total time.
  double timeout = 0.1;
  double timeEnd = cmSystemTools::GetTime() + timeout;
  std::string line;
  while((timeout = timeEnd - cmSystemTools::GetTime(), timeout > 0))
    {
    int p = this->TestProcess->GetNextOutputLine(line, timeout);
    if(p == cmsysProcess_Pipe_None)
      {
      // Process has terminated and all output read.
      return false;
      }
    else if(p == cmsysProcess_Pipe_STDOUT ||
            p == cmsysProcess_Pipe_STDERR)
      {
      // Store this line of output.
      cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                 this->GetIndex() << ": " << line << std::endl);  
      this->ProcessOutput += line;
      this->ProcessOutput += "\n";
      }
    else // if(p == cmsysProcess_Pipe_Timeout)
      {
      break;
      }
    }
  return true;
}

//---------------------------------------------------------
// Streamed compression of test output.  The compressed data
// is appended to this->CompressedOutput
void cmCTestRunTest::CompressOutput()
{
  int ret;
  z_stream strm;

  unsigned char* in = 
    reinterpret_cast<unsigned char*>(
    const_cast<char*>(this->ProcessOutput.c_str()));
  //zlib makes the guarantee that this is the maximum output size
  int outSize = static_cast<int>(this->ProcessOutput.size() * 1.001 + 13);
  unsigned char* out = new unsigned char[outSize];

  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  ret = deflateInit(&strm, -1); //default compression level
  if (ret != Z_OK)
    {
    return;
    }

  strm.avail_in = static_cast<uInt>(this->ProcessOutput.size());
  strm.next_in = in;
  strm.avail_out = outSize;
  strm.next_out = out;
  ret = deflate(&strm, Z_FINISH);

  if(ret == Z_STREAM_ERROR || ret != Z_STREAM_END)
    {
    cmCTestLog(this->CTest, ERROR_MESSAGE, "Error during output "
      "compression. Sending uncompressed output." << std::endl);
    return;
    }

  (void)deflateEnd(&strm);
 
  unsigned char *encoded_buffer
    = new unsigned char[static_cast<int>(outSize * 1.5)];

  unsigned long rlen
    = cmsysBase64_Encode(out, strm.total_out, encoded_buffer, 1);

  for(unsigned long i = 0; i < rlen; i++)
    {
    this->CompressedOutput += encoded_buffer[i];
    }

  if(strm.total_in)
    {
    this->CompressionRatio = static_cast<double>(strm.total_out) /
                             static_cast<double>(strm.total_in);
    }

  delete [] encoded_buffer;
  delete [] out;
}

//---------------------------------------------------------
bool cmCTestRunTest::EndTest(size_t completed, size_t total, bool started)
{
  if (this->CTest->ShouldCompressTestOutput())
    {
    this->CompressOutput();
    }

  //restore the old environment
  if (this->ModifyEnv)
    {
    cmSystemTools::RestoreEnv(this->OrigEnv);
    }
  this->WriteLogOutputTop(completed, total);
  std::string reason;
  bool passed = true;
  int res = started ? this->TestProcess->GetProcessStatus()
                    : cmsysProcess_State_Error;
  int retVal = this->TestProcess->GetExitValue();
  std::vector<std::pair<cmsys::RegularExpression,
    std::string> >::iterator passIt;
  bool forceFail = false;
  bool outputTestErrorsToConsole = false;
  if ( this->TestProperties->RequiredRegularExpressions.size() > 0 )
    {
    bool found = false;
    for ( passIt = this->TestProperties->RequiredRegularExpressions.begin();
          passIt != this->TestProperties->RequiredRegularExpressions.end();
          ++ passIt )
      {
      if ( passIt->first.find(this->ProcessOutput.c_str()) )
        {
        found = true;
        reason = "Required regular expression found.";
        }
      }
    if ( !found )
      { 
      reason = "Required regular expression not found.";
      forceFail = true;
      }
    reason +=  "Regex=["; 
    for ( passIt = this->TestProperties->RequiredRegularExpressions.begin();
          passIt != this->TestProperties->RequiredRegularExpressions.end();
          ++ passIt )
      {
      reason += passIt->second;
      reason += "\n";
      }
    reason += "]";
    }
  if ( this->TestProperties->ErrorRegularExpressions.size() > 0 )
    {
    for ( passIt = this->TestProperties->ErrorRegularExpressions.begin();
          passIt != this->TestProperties->ErrorRegularExpressions.end();
          ++ passIt )
      {
      if ( passIt->first.find(this->ProcessOutput.c_str()) )
        {
        reason = "Error regular expression found in output.";
        reason += " Regex=[";
        reason += passIt->second;
        reason += "]";
        forceFail = true;
        }
      }
    }
  if (res == cmsysProcess_State_Exited)
    {
    bool success = 
      !forceFail &&  (retVal == 0 || 
      this->TestProperties->RequiredRegularExpressions.size());
    if((success && !this->TestProperties->WillFail) 
      || (!success && this->TestProperties->WillFail))
      {
      this->TestResult.Status = cmCTestTestHandler::COMPLETED;
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "   Passed  " );
      }
    else
      {
      this->TestResult.Status = cmCTestTestHandler::FAILED;
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Failed  " << reason );
      outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
      }
    }
  else if ( res == cmsysProcess_State_Expired )
    {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Timeout ");
    this->TestResult.Status = cmCTestTestHandler::TIMEOUT;
    outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
    }
  else if ( res == cmsysProcess_State_Exception )
    {
    outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Exception: ");
    switch ( retVal )
      {
      case cmsysProcess_Exception_Fault:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "SegFault");
        this->TestResult.Status = cmCTestTestHandler::SEGFAULT;
        break;
      case cmsysProcess_Exception_Illegal:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Illegal");
        this->TestResult.Status = cmCTestTestHandler::ILLEGAL;
        break;
      case cmsysProcess_Exception_Interrupt:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Interrupt");
        this->TestResult.Status = cmCTestTestHandler::INTERRUPT;
        break;
      case cmsysProcess_Exception_Numerical:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Numerical");
        this->TestResult.Status = cmCTestTestHandler::NUMERICAL;
        break;
      default:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Other");
        this->TestResult.Status = cmCTestTestHandler::OTHER_FAULT;
      }
    }
  else //cmsysProcess_State_Error
    {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Not Run ");
    }

  passed = this->TestResult.Status == cmCTestTestHandler::COMPLETED;
  char buf[1024];
  sprintf(buf, "%6.2f sec", this->TestProcess->GetTotalTime());
  cmCTestLog(this->CTest, HANDLER_OUTPUT, buf << "\n" );

  if ( outputTestErrorsToConsole )
    {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, this->ProcessOutput << std::endl );
    }

  if ( this->TestHandler->LogFile )
    {
    *this->TestHandler->LogFile << "Test time = " << buf << std::endl;
    }
  this->DartProcessing();
  // if this is doing MemCheck then all the output needs to be put into
  // Output since that is what is parsed by cmCTestMemCheckHandler
  if(!this->TestHandler->MemCheck && started)
    {
      this->TestHandler->CleanTestOutput(this->ProcessOutput, 
          static_cast<size_t>
          (this->TestResult.Status == cmCTestTestHandler::COMPLETED ? 
          this->TestHandler->CustomMaximumPassedTestOutputSize :
          this->TestHandler->CustomMaximumFailedTestOutputSize));
    }
  this->TestResult.Reason = reason;
  if (this->TestHandler->LogFile)
    {
    bool pass = true;
    const char* reasonType = "Test Pass Reason";
    if(this->TestResult.Status != cmCTestTestHandler::COMPLETED &&
       this->TestResult.Status != cmCTestTestHandler::NOT_RUN)
      {
      reasonType = "Test Fail Reason";
      pass = false;
      }
    double ttime = this->TestProcess->GetTotalTime();
    int hours = static_cast<int>(ttime / (60 * 60));
    int minutes = static_cast<int>(ttime / 60) % 60;
    int seconds = static_cast<int>(ttime) % 60;
    char buffer[100];
    sprintf(buffer, "%02d:%02d:%02d", hours, minutes, seconds);
    *this->TestHandler->LogFile
      << "----------------------------------------------------------"
      << std::endl;
    if(this->TestResult.Reason.size())
      {
      *this->TestHandler->LogFile << reasonType << ":\n" 
        << this->TestResult.Reason << "\n";
      }
    else 
      {
      if(pass)
        {
        *this->TestHandler->LogFile << "Test Passed.\n";
        }
      else
        {
        *this->TestHandler->LogFile << "Test Failed.\n";
        }
      }
    *this->TestHandler->LogFile << "\"" << this->TestProperties->Name.c_str()
      << "\" end time: " << this->CTest->CurrentTime() << std::endl
      << "\"" << this->TestProperties->Name.c_str() << "\" time elapsed: "
      << buffer << std::endl
      << "----------------------------------------------------------"
      << std::endl << std::endl;
    }
  // if the test actually started and ran 
  // record the results in TestResult 
  if(started)
    {
    bool compress = this->CompressionRatio < 1 &&
      this->CTest->ShouldCompressTestOutput();
    this->TestResult.Output = compress ? this->CompressedOutput 
      : this->ProcessOutput;
    this->TestResult.CompressOutput = compress;
    this->TestResult.ReturnValue = this->TestProcess->GetExitValue();
    this->TestResult.CompletionStatus = "Completed";
    this->TestResult.ExecutionTime = this->TestProcess->GetTotalTime();
    this->MemCheckPostProcess();
    this->ComputeWeightedCost();
    }
  // Always push the current TestResult onto the
  // TestHandler vector
  this->TestHandler->TestResults.push_back(this->TestResult);
  delete this->TestProcess;
  return passed;
}

//----------------------------------------------------------------------
void cmCTestRunTest::ComputeWeightedCost()
{
  int prev = this->TestProperties->PreviousRuns;
  float avgcost = this->TestProperties->Cost;
  double current = this->TestResult.ExecutionTime;

  if(this->TestResult.Status == cmCTestTestHandler::COMPLETED)
    {
    this->TestProperties->Cost = ((prev * avgcost) + current) / (prev + 1);
    this->TestProperties->PreviousRuns++;
    }
}

//----------------------------------------------------------------------
void cmCTestRunTest::MemCheckPostProcess()
{
  if(!this->TestHandler->MemCheck)
    {
    return;
    }
  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index 
             << ": process test output now: "
             << this->TestProperties->Name.c_str() << " "
             << this->TestResult.Name.c_str() << std::endl);
  cmCTestMemCheckHandler * handler = static_cast<cmCTestMemCheckHandler*>
    (this->TestHandler);
  if(handler->MemoryTesterStyle == cmCTestMemCheckHandler::BOUNDS_CHECKER)
    {
    handler->PostProcessBoundsCheckerTest(this->TestResult);
    }
  else if(handler->MemoryTesterStyle == cmCTestMemCheckHandler::PURIFY)
    {
    handler->PostProcessPurifyTest(this->TestResult); 
    }
}

//----------------------------------------------------------------------
// Starts the execution of a test.  Returns once it has started
bool cmCTestRunTest::StartTest(size_t total)
{
  cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(2*getNumWidth(total) + 8)
    << "Start "
    << std::setw(getNumWidth(this->TestHandler->GetMaxIndex()))
    << this->TestProperties->Index << ": "
    << this->TestProperties->Name << std::endl);
  this->ComputeArguments();
  std::vector<std::string>& args = this->TestProperties->Args;
  this->TestResult.Properties = this->TestProperties;
  this->TestResult.ExecutionTime = 0;
  this->TestResult.CompressOutput = false;
  this->TestResult.ReturnValue = -1;
  this->TestResult.CompletionStatus = "Failed to start";
  this->TestResult.Status = cmCTestTestHandler::BAD_COMMAND;
  this->TestResult.TestCount = this->TestProperties->Index;  
  this->TestResult.Name = this->TestProperties->Name;
  this->TestResult.Path = this->TestProperties->Directory.c_str();
  
  // Check if all required files exist
  for(std::vector<std::string>::iterator i =
    this->TestProperties->RequiredFiles.begin();
    i != this->TestProperties->RequiredFiles.end(); ++i)
    {
    std::string file = *i;

    if(!cmSystemTools::FileExists(file.c_str()))
      {
      //Required file was not found
      this->TestProcess = new cmProcess;
      *this->TestHandler->LogFile << "Unable to find required file: "
               << file.c_str() << std::endl;
      cmCTestLog(this->CTest, ERROR_MESSAGE, "Unable to find required file: "
               << file.c_str() << std::endl);
      this->TestResult.Output = "Unable to find required file: " + file;
      this->TestResult.FullCommandLine = "";
      this->TestResult.CompletionStatus = "Not Run";
      this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
      return false;
      }
    }
  // log and return if we did not find the executable
  if (this->ActualCommand == "")
    {
    // if the command was not found create a TestResult object
    // that has that information 
    this->TestProcess = new cmProcess;
    *this->TestHandler->LogFile << "Unable to find executable: " 
                   << args[1].c_str() << std::endl;
    cmCTestLog(this->CTest, ERROR_MESSAGE, "Unable to find executable: "
               << args[1].c_str() << std::endl);
    this->TestResult.Output = "Unable to find executable: " + args[1];
    this->TestResult.FullCommandLine = "";
    this->TestResult.CompletionStatus = "Not Run";
    this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
    return false;
    }
  this->StartTime = this->CTest->CurrentTime();

  return this->ForkProcess(this->ResolveTimeout(),
                             &this->TestProperties->Environment);
}

//----------------------------------------------------------------------
void cmCTestRunTest::ComputeArguments()
{
  std::vector<std::string>::const_iterator j =
    this->TestProperties->Args.begin();
  ++j; // skip test name

  // find the test executable
  if(this->TestHandler->MemCheck)
    {
    cmCTestMemCheckHandler * handler = static_cast<cmCTestMemCheckHandler*>
      (this->TestHandler);
    this->ActualCommand = handler->MemoryTester.c_str();
    this->TestProperties->Args[1] = this->TestHandler->FindTheExecutable(
      this->TestProperties->Args[1].c_str());
    }
  else
    {
    this->ActualCommand =
      this->TestHandler->FindTheExecutable(
      this->TestProperties->Args[1].c_str());
    ++j; //skip the executable (it will be actualCommand)
    }
  this->TestCommand
    = cmSystemTools::ConvertToOutputPath(this->ActualCommand.c_str());

  //Prepends memcheck args to our command string
  this->TestHandler->GenerateTestCommand(this->Arguments);
  for(std::vector<std::string>::iterator i = this->Arguments.begin();
      i != this->Arguments.end(); ++i)
    {
    this->TestCommand += " ";
    this->TestCommand += cmSystemTools::EscapeSpaces(i->c_str());
    }

  for(;j != this->TestProperties->Args.end(); ++j)
    {
    this->TestCommand += " ";
    this->TestCommand += cmSystemTools::EscapeSpaces(j->c_str());
    this->Arguments.push_back(*j);
    }
  this->TestResult.FullCommandLine = this->TestCommand;

  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, std::endl
             << this->Index << ": "
             << (this->TestHandler->MemCheck?"MemCheck":"Test") 
             << " command: " << this->TestCommand
             << std::endl);
}

//----------------------------------------------------------------------
void cmCTestRunTest::DartProcessing()
{
  if (!this->ProcessOutput.empty() && 
     this->ProcessOutput.find("<DartMeasurement") != this->ProcessOutput.npos)
    {
    if (this->TestHandler->DartStuff.find(this->ProcessOutput.c_str()))
      {
      std::string dartString = this->TestHandler->DartStuff.match(1);
      // keep searching and replacing until none are left
      while (this->TestHandler->DartStuff1.find(this->ProcessOutput.c_str()))
        {
        // replace the exact match for the string
        cmSystemTools::ReplaceString(this->ProcessOutput,
                         this->TestHandler->DartStuff1.match(1).c_str(), "");
        }
      this->TestResult.RegressionImages
        = this->TestHandler->GenerateRegressionImages(dartString);
      }
    }
}

//----------------------------------------------------------------------
double cmCTestRunTest::ResolveTimeout()
{
  double timeout = this->TestProperties->Timeout;

  if(this->CTest->GetStopTime() == "")
    {
    return timeout;
    }
  struct tm* lctime;
  time_t current_time = time(0);
  lctime = gmtime(&current_time);
  int gm_hour = lctime->tm_hour;
  time_t gm_time = mktime(lctime);
  lctime = localtime(&current_time);
  int local_hour = lctime->tm_hour;

  int tzone_offset = 0;
  if(gm_time > current_time && gm_hour < local_hour)
    {
    // this means gm_time is on the next day
    tzone_offset = local_hour - gm_hour - 24;
    }
  else
    {
    tzone_offset = local_hour - gm_hour;
    }

  tzone_offset *= 100;
  char buf[1024];
  // add todays year day and month to the time in str because
  // curl_getdate no longer assumes the day is today
  sprintf(buf, "%d%02d%02d %s %+05i",
          lctime->tm_year + 1900,
          lctime->tm_mon + 1,
          lctime->tm_mday,
          this->CTest->GetStopTime().c_str(),
          tzone_offset);

  time_t stop_time = curl_getdate(buf, &current_time);
  if(stop_time == -1)
    {
    return timeout;
    }

  //the stop time refers to the next day
  if(this->CTest->NextDayStopTime)
    {
    stop_time += 24*60*60;
    }
  int stop_timeout = (stop_time - current_time) % (24*60*60);

  if(stop_timeout <= 0)
    {
    cmCTestLog(this->CTest, ERROR_MESSAGE, "The stop time has been passed. "
      "Exiting ctest." << std::endl);
    exit(-1);
    }
#undef min
  return timeout == 0 ? stop_timeout :
    std::min(timeout, static_cast<double>(stop_timeout));
}

//----------------------------------------------------------------------
bool cmCTestRunTest::ForkProcess(double testTimeOut,
                     std::vector<std::string>* environment)
{
  this->TestProcess = new cmProcess;
  this->TestProcess->SetId(this->Index);
  this->TestProcess->SetWorkingDirectory(
        this->TestProperties->Directory.c_str());
  this->TestProcess->SetCommand(this->ActualCommand.c_str());
  this->TestProcess->SetCommandArguments(this->Arguments);

  std::vector<std::string> origEnv;
  this->ModifyEnv = (environment && environment->size()>0);

  // determine how much time we have
  double timeout = this->CTest->GetRemainingTimeAllowed() - 120;
  if (this->CTest->GetTimeOut() > 0 && this->CTest->GetTimeOut() < timeout)
    {
    timeout = this->CTest->GetTimeOut();
    }
  if (testTimeOut > 0
      && testTimeOut < this->CTest->GetRemainingTimeAllowed())
    {
    timeout = testTimeOut;
    }

  // always have at least 1 second if we got to here
  if (timeout <= 0)
    {
    timeout = 1;
    }
  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index << ": "
             << "Test timeout computed to be: " << timeout << "\n");

  this->TestProcess->SetTimeout(timeout);

  if (this->ModifyEnv)
    {
    this->OrigEnv = cmSystemTools::AppendEnv(environment);
    }

  return this->TestProcess->StartProcess();
}

void cmCTestRunTest::WriteLogOutputTop(size_t completed, size_t total)
{
  cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
             << completed << "/");
  cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
             << total << " ");

  if ( this->TestHandler->MemCheck )
    {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "MemCheck");
    }
  else
    {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "Test");
    }

  cmOStringStream indexStr;
  indexStr << " #" << this->Index << ":";
  cmCTestLog(this->CTest, HANDLER_OUTPUT, 
             std::setw(3 + getNumWidth(this->TestHandler->GetMaxIndex()))
             << indexStr.str().c_str());
  cmCTestLog(this->CTest, HANDLER_OUTPUT, " ");
  const int maxTestNameWidth = this->CTest->GetMaxTestNameWidth();
  std::string outname = this->TestProperties->Name + " ";
  outname.resize(maxTestNameWidth + 4, '.');

  *this->TestHandler->LogFile << this->TestProperties->Index << "/"
    << this->TestHandler->TotalNumberOfTests << " Testing: " 
    << this->TestProperties->Name << std::endl;
  *this->TestHandler->LogFile << this->TestProperties->Index << "/"
    << this->TestHandler->TotalNumberOfTests
    << " Test: " << this->TestProperties->Name.c_str() << std::endl;
  *this->TestHandler->LogFile << "Command: \"" << this->ActualCommand << "\"";
  
  for (std::vector<std::string>::iterator i = this->Arguments.begin();
       i != this->Arguments.end(); ++i)
    {
    *this->TestHandler->LogFile 
      << " \"" << i->c_str() << "\"";
    }
  *this->TestHandler->LogFile << std::endl
    << "Directory: " << this->TestProperties->Directory << std::endl
    << "\"" << this->TestProperties->Name.c_str() << "\" start time: "
    << this->StartTime << std::endl;

  *this->TestHandler->LogFile
    << "Output:" << std::endl
    << "----------------------------------------------------------"
    << std::endl;
  *this->TestHandler->LogFile
    << this->ProcessOutput.c_str() << "<end of output>" << std::endl;

  cmCTestLog(this->CTest, HANDLER_OUTPUT, outname.c_str());
  cmCTestLog(this->CTest, DEBUG, "Testing " 
             << this->TestProperties->Name.c_str() << " ... ");
}

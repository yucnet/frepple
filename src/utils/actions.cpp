/***************************************************************************
  file : $URL: file:///develop/SVNrepository/frepple/trunk/src/utils/actions.cpp $
  version : $LastChangedRevision$  $LastChangedBy$
  date : $LastChangedDate$
  email : jdetaeye@users.sourceforge.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 * Copyright (C) 2006 by Johan De Taeye                                    *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation; either version 2.1 of the License, or  *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser *
 * General Public License for more details.                                *
 *                                                                         *
 * You should have received a copy of the GNU Lesser General Public        *
 * License along with this library; if not, write to the Free Software     *
 * Foundation Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE 
#include "frepple/utils.h"


// These headers are required for the loading of dynamic libraries
#ifdef WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif


namespace frepple
{


bool Command::getVerbose() const
{
  if (verbose==INHERIT)
    // Note: a command gets the level INHERIT by default. In case the command
    // was never added to a commandlist, the owner field will be NULL. In such
    // case the value INHERIT is interpreted as SILENT.
    return owner ? owner->getVerbose() : false;
  else
    return verbose==YES;
}


void Command::endElement(XMLInput& pIn, XMLElement& pElement)
{
  if (pElement.isA(Tags::tag_verbose)) setVerbose(pElement.getBool());
}


//
// COMMAND LIST
//


bool CommandList::getAbortOnError() const
{
  if (abortOnError==INHERIT)
  {
    // A command list can be nested in another command list. In this case we
    // inherit this field from the owning command list
    CommandList *owning_list = dynamic_cast<CommandList*>(owner);
    return owning_list ? owning_list->getAbortOnError() : true;
  }
  else
    return abortOnError==YES;
}


void CommandList::add(Command* c)
{
  if (!c) throw LogicException("Adding NULL command to a command list");
  c->owner = this;

  // Maintenance of the linked list of child commands
  if (lastCommand)
  {
    // Let the last command in the chain point to this new extra command
    lastCommand->next = c;
    lastCommand = c;
  }
  else
  {
    // This is the first command in this command list
    firstCommand = c;
    lastCommand = c;
  }

  // Update the undoable field
  if(!c->undoable()) can_undo=false;
}


void CommandList::undo()
{
  // Don't even try to undo a list which can't be undone.
  if (!can_undo)
    throw RuntimeException("Trying to undo an action list which " \
      "contains non-undoable actions.");

  // Undo all commands and delete them.
  // Note that undoing an operation that hasn't been executed yet or has been
  // undone already is expected to be harmless, so we don't need to worry
  // about that...
  for(Command *i=firstCommand; i; )
  {
    i->undo();
    Command *t = i;  // Temporarily store the pointer to be deleted
    i = i->next;
    delete t;
  }
  firstCommand = NULL;
  lastCommand = NULL;
}


void CommandList::execute()
{
  // Log
  if (getVerbose())
    clog << "Start executing command list at " << Date::now() << endl;
  Timer t;

  // Execute the actions
  Command *i = firstCommand;

#ifndef MT
  if (!sequential)
  {
    // MODE 1: Parallel execution of the commands, is not possible
    clog << "Warning: Parallel execution of commands is not supported "
     << "with this executable." << endl
     << "Defaulting to sequential processing." << endl;
    sequential = true;
  }
#else
  if (!sequential)
  {
    // MODE 1: Parallel execution of the commands
    int numthreads = size();
    if(numthreads == 1)
      // Only a single command in the list: no need for threads
      CommandThread(i);
    else if (numthreads > 1)
    {
      // Create a thread for every command list. The main thread will then
      // wait for all of them to finish.
      pthread_t threads[numthreads];     // holds thread info
      int errcode;                       // holds pthread error code
      int status;                        // holds return value

      // Create the command threads
      /** @todo implement a maximum on the number of parallel commands. */
      for (int worker=0; worker<numthreads; ++worker)
      {
        if ((errcode=pthread_create(&threads[worker],  // thread struct
                                NULL,              // default thread attributes
                                CommandThread,     // start routine
                                i)))               // arg to routine
        {
          ostringstream ch;
          ch << "Couldn't create thread " << worker << ", error " << errcode;
          throw RuntimeException(ch.str());
        }
        i = i->next;
    }

    // Wait for the command threads as they exit
    for (int worker=0; worker<numthreads; ++worker)
      // Wait for thread to terminate
      if ((errcode=pthread_join(threads[worker],(void**) &status)))
      {
        ostringstream ch;
        ch << "Couldn't join with thread " << worker << ", error " << errcode;
        throw RuntimeException(ch.str());
      }
    }
  } else
#endif
  if (getAbortOnError())
  {
    // MODE 2: Sequential execution, and a single command failure aborts the
    // whole sequence.
    try
    {
      for(; i; i=i->next) i->execute();
    }
    catch (...)
    {
      clog << "Error: Caught an exception while executing command '"
        << i->getDescription() << "':" <<  endl;
      try { throw; }
      catch (exception& e) {clog << "  " << e.what() << endl;}
      catch (...) {clog << "  Unknown type" << endl;}
      // Undo all commands executed so far
      if (undoable()) undo();
    }
  }
  else
    // MODE 3: Sequential execution, and when a command in the sequence fails
    // the rest continues
    for(; i; i=i->next) CommandThread(i);

  // Clean it up after executing ALL actions.
  for(i=firstCommand; i; )
  {
    Command *t = i;
    i = i->next;
    delete t;
  }
  firstCommand = NULL;
  lastCommand = NULL;

  // Log
  if (getVerbose())
    clog << "Finished executing command list at " << Date::now()
      << " : " << t << endl;
}


void* CommandList::CommandThread(void *arg)
{
  // Execute the command
  Command *c = static_cast<Command*>(arg);
  try {c->execute();}
  catch (...)
  {
    clog << "Error: Caught an exception while executing command '"
      << c->getDescription() << "':" <<  endl;
    try { throw; }
    catch (exception& e) {clog << "  " << e.what() << endl;}
    catch (...) {clog << "  Unknown type" << endl;}
    // Undo the commands executed so far
    if (c->undoable()) c->undo();
  }
  // Return value doesn't matter. The POSIX pthread interface wants a void
  // pointer as return value, so we just pass NULL...
  return NULL;
}


CommandList::~CommandList()
{
  if (!firstCommand) return;
  clog << "Warning: Deleting an action list with actions that have"
    << " not been committed or undone" << endl;
  for(Command *i = firstCommand; i; )
  {
    Command *t = i;  // Temporary storage for the object to delete
    i = i->next;
    delete t;
  }
}


void CommandList::endElement(XMLInput& pIn, XMLElement& pElement)
{
  if (pElement.isA(Tags::tag_command) && !pIn.isObjectEnd())
  {
    // We're unlucky with our tag names here. Subcommands end with
    // </COMMAND>, but the command list itself also ends with that tag.
    // We use the isObjectEnd() method to differentiate between both.
    Command * b = dynamic_cast<Command*>(pIn.getPreviousObject());
    if (b) add(b);
    else throw LogicException("Incorrect object type during read operation");
  }
  else if (pElement.isA(Tags::tag_abortonerror))
    setAbortOnError(pElement.getBool());
  else if (pElement.isA(Tags::tag_sequential))
    setSequential(pElement.getBool());
  else
    Command::endElement(pIn, pElement);
}


void CommandList::beginElement (XMLInput& pIn, XMLElement& pElement)
{
  if (pElement.isA (Tags::tag_command))
    pIn.readto( MetaCategory::ControllerDefault(Command::metadata,pIn.getAttributes()) );
}


//
// SYSTEM COMMAND
//


void CommandSystem::execute()
{
  // Log
  if (getVerbose())
    clog << "Start executing system command '" << cmdLine
    << "' at " << Date::now() << endl;
  Timer t;

  // Execute
  if (cmdLine.empty())
    throw DataException("Error: Trying to execute empty system command");
  else if (system(cmdLine.c_str()))  // Execution through system() call
    throw RuntimeException("Error while executing system command: " + cmdLine);

  // Log
  if (getVerbose())
    clog << "Finished executing system command '" << cmdLine
    << "' at " << Date::now() << " : " << t << endl;
}


void CommandSystem::endElement(XMLInput& pIn, XMLElement& pElement)
{
  if (pElement.isA(Tags::tag_cmdline))
    pElement >> cmdLine;
  else
    Command::endElement(pIn, pElement);
}


//
// LOADLIBRARY COMMAND
//


void CommandLoadLibrary::execute()
{
  // Type definition of the initialization function
  typedef void (*func)(const ParameterList&);

  // Log
  if (getVerbose())
    clog << "Start loading library '" << lib << "' at " << Date::now() << endl;
  Timer t;

  // Validate
  if (lib.empty())
    throw DataException("Error: No library name specified for loading");

#ifdef WIN32
  // Load the library - The windows way
  // Change the error mode: we handle errors now, not the operating system
  UINT em = SetErrorMode(SEM_FAILCRITICALERRORS);
  HINSTANCE handle = LoadLibrary(lib.c_str());
  if (!handle) 
  {
    // Get the error description
    char error[256];
    FormatMessage(
      FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, 
      GetLastError(),
      0,
      error, 
      256, 
      NULL );
    throw RuntimeException(error);
  }
  SetErrorMode(em);  // Restore the previous error mode

  // Find the initialization routine
  func inithandle =
    reinterpret_cast<func>(GetProcAddress(HMODULE(handle), "initialize"));
  if (!inithandle) 
  {
    // Get the error description
    char error[256];
    FormatMessage(
      FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, 
      GetLastError(),
      0,
      error, 
      256, 
      NULL );
    throw RuntimeException(error);
  }

#else
  // Load the library - The unix way
  dlerror(); // Clear the previous error
  void *handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
  const char *err = dlerror();  // Pick up the error string
  if (err) throw RuntimeException(err);

  // Find the initialization routine
  func inithandle = reinterpret_cast<func>(dlsym(handle, "initialize"));
  err = dlerror(); // Pick up the error string
  if (err) throw RuntimeException(err);
#endif

  // Call the initialization routine with the parameter list 
  (inithandle)(parameters);

  // Log
  if (getVerbose())
    clog << "Finished loading library '" << lib
      << "' at " << Date::now() << " : " << t << endl;
}


void CommandLoadLibrary::endElement(XMLInput& pIn, XMLElement& pElement)
{
  if (pElement.isA(Tags::tag_filename))
    pElement >> lib;
  else if (pElement.isA(Tags::tag_name))
    pElement >> tempName;
  else if (pElement.isA(Tags::tag_value))
    pElement >> tempValue;
  else if (pElement.isA(Tags::tag_parameter))
  {
    if (!tempValue.empty() && !tempName.empty())
    {
      // New parameter name+value pair ready
      parameters[tempName] = tempValue;
      tempValue.clear();
      tempName.clear();
    }
    else 
      // Incomplete data
      throw DataException("Invalid parameter specification");
  }
  else if (pIn.isObjectEnd())
  {
    tempValue.clear();
    tempName.clear();
  }
  else
    Command::endElement(pIn, pElement);
}


}

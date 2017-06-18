#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <sys/user.h>

#include "trace.h"

#define HIGH_BIT_MASK (~((unsigned long)0xFF))

static bool
trace_info_initialize(program_arguments_t *args, trace_info_t *trace_info)
{
  trace_info->prog_fd = -1;
  trace_info->function_infos = calloc(args->num_functions,
                                      sizeof(*trace_info->function_infos));
  trace_info->num_func_infos = args->num_functions;
  trace_info->next_func = 0;

  return trace_info->function_infos != NULL;
}

static void
trace_info_free(trace_info_t *trace_info)
{
  free(trace_info->function_infos);
  if(trace_info->prog_fd != -1) {
    close(trace_info->prog_fd);
  }

  trace_info->function_infos = NULL;
  trace_info->prog_fd = -1;
}

static bool
trace_is_target_function(program_arguments_t *args,
                         trace_info_t *trace_info,
                         Dwarf_Die die)
{
  Dwarf_Error err;
  Dwarf_Half tag;
  Dwarf_Attribute attr;
  Dwarf_Addr addr;
  char *name;
  int i;
  int res;

  (void) trace_info;

  if(dwarf_tag(die, &tag, &err) != DW_DLV_OK) {
    fprintf(stderr, "dwarf_tag failed\n");
    return -1;
  }

  if(tag != DW_TAG_subprogram) {
    return 0;
  }

  res = dwarf_attr(die, DW_AT_name, &attr, &err);

  if(res == DW_DLV_ERROR) {
    fprintf(stderr, "dwarf_attr failed\n");
    return -1;
  }

  res = dwarf_formstring(attr, &name, &err);

  if(res == DW_DLV_ERROR) {
    fprintf(stderr, "dwarf_formstring failed\n");
    return -1;
  }

  for(i = 0; i < args->num_functions; i++) {
    if(strcmp(args->functions[i], name) == 0) {
      trace_info->function_infos[trace_info->next_func].name = strdup(name);
      if(trace_info->function_infos[trace_info->next_func].name == NULL) {
        return -1;
      }
      if(dwarf_lowpc(die, &addr, &err) == DW_DLV_ERROR) {
        fprintf(stderr, "Failed to get low pc\n");
        free(trace_info->function_infos[trace_info->next_func].name);
        trace_info->function_infos[trace_info->next_func].name = NULL;
        return -1;
      }
      trace_info->function_infos[trace_info->next_func].ip = addr;
      return 1;
    }
  }

  return 0;
}

static int
trace_cu_find_functions(program_arguments_t *args,
                        trace_info_t *trace_info,
                        Dwarf_Die cu_die)
{
  int count = 0;
  Dwarf_Error err;
  Dwarf_Die child_die;
  int res;

  res = dwarf_child(cu_die, &child_die, &err);

  if(res == DW_DLV_ERROR) {
    fprintf(stderr, "dwarf_child failed in %s\n", __FUNCTION__);
    return -1;
  }

  while(1) {

    res = trace_is_target_function(args, trace_info, child_die);

    if(res < 0) {
      return -1;
    }
    else if(res == 1) {
      trace_info->next_func++;
      count++;
    }

    res = dwarf_siblingof(trace_info->dbg, child_die, &child_die, &err);

    if(res == DW_DLV_ERROR) {
      fprintf(stderr, "dwarf_siblingof failed in %s\n", __FUNCTION__);
      return -1;
    }
    else if(res == DW_DLV_NO_ENTRY) {
      break;
    }
  }

  return count;
}

bool
trace_find_functions(program_arguments_t *args, trace_info_t *trace_info)
{
  int res;
  Dwarf_Die previous_die = 0;
  Dwarf_Die cu_die;
  Dwarf_Error err;
  Dwarf_Unsigned cu_header_length;
  Dwarf_Unsigned abbrev_offset;
  Dwarf_Unsigned next_cu_header;
  Dwarf_Half version_stamp;
  Dwarf_Half address_size;
  int found_functions;
  int total_found_functions = 0;

  trace_info_initialize(args, trace_info);

  trace_info->prog_fd = open(args->child_args[0], O_RDONLY);

  if(trace_info->prog_fd < 0) {
    fprintf(stderr, "Failed to open %s\n", args->child_args[0]);
    goto failure;
  }

  if(dwarf_init(trace_info->prog_fd, DW_DLC_READ, 0, 0,
                &trace_info->dbg, &err) != DW_DLV_OK) {
    fprintf(stderr, "Failed DWARF initialization\n");
    goto failure;
  }

  while(((res = dwarf_next_cu_header(trace_info->dbg, &cu_header_length,
                                     &version_stamp, &abbrev_offset,
                                     &address_size, &next_cu_header, &err))
         != DW_DLV_NO_ENTRY) &&
        (total_found_functions < trace_info->num_func_infos)) {

    if(res == DW_DLV_ERROR) {
      fprintf(stderr, "dwarf_next_cu_header failed\n");
      goto failure;
    }

    previous_die = 0;
    while((res = dwarf_siblingof(trace_info->dbg, previous_die,
                                 &cu_die, &err)) != DW_DLV_NO_ENTRY) {

      if(res == DW_DLV_ERROR) {
        fprintf(stderr, "Failed to find sibling of CU die\n");
        goto failure;
      }

      found_functions = trace_cu_find_functions(args, trace_info, cu_die);
      if(found_functions < 0) {
        goto failure;
      }
      total_found_functions += found_functions;
      previous_die = cu_die;
    }
  }

  if(trace_info->num_func_infos != total_found_functions) {
    goto failure;
  }

  return true;

 failure:
  trace_info_free(trace_info);
  return false;
}

static bool
trace_add_breakpoint(pid_t pid, function_information_t *func_info) {

  errno = 0;

  func_info->original = ptrace(PTRACE_PEEKTEXT, pid, (void *)func_info->ip, 0);

  if(errno) {
    return false;
  }

  ptrace(PTRACE_POKETEXT, pid, (void *)func_info->ip,
         (func_info->original & HIGH_BIT_MASK) | 0xCC);

  return errno == 0;
}

bool
trace_add_breakpoints(trace_info_t *trace_info)
{
  int i;

  for(i = 0; i < trace_info->num_func_infos; i++) {
    if(!trace_add_breakpoint(trace_info->child_pid,
                             &(trace_info->function_infos[i]))) {
      kill(trace_info->child_pid, SIGKILL);
      return false;
    }
  }

  ptrace(PTRACE_CONT, trace_info->child_pid, NULL, NULL);

  return true;
}

bool
trace_launch(program_arguments_t *args, trace_info_t *trace_info)
{
  int status;
  int res;

  trace_info->child_pid = fork();

  if(trace_info->child_pid == -1) {
    return false;
  }

  if(trace_info->child_pid) {
    /* parent */

    res = waitpid(trace_info->child_pid, &status, 0);
    return res != -1 && ((!WIFEXITED(status)) && (!WIFSIGNALED(status)));
  }
  else {
    /* child */

    close(trace_info->prog_fd);
    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    execvp(args->child_args[0], args->child_args);

    fprintf(stderr, "exec failed: errno %d\n", errno);
    exit(EXIT_FAILURE);
  }

  return false;
}

static bool
trace_get_child_ip(trace_info_t *trace_info, unsigned long *ip)
{
  struct user_regs_struct regs;

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "Failed to get child ip\n");
    return false;
  }

  *ip = regs.rip;

  return true;
}

static bool
trace_restore_function(trace_info_t *trace_info,
                       function_information_t *func_info)
{
  long data;

  errno = 0;
  data = ptrace(PTRACE_PEEKTEXT, trace_info->child_pid,
                (void*)func_info->ip, 0);

  if(errno) {
    return false;
  }

  ptrace(PTRACE_POKETEXT, trace_info->child_pid, (void*)func_info->ip,
         (data & HIGH_BIT_MASK) | (func_info->original & 0xFF));

  if(errno) {
    return false;
  }

  return true;
}

static bool
trace_get_return_address(trace_info_t *trace_info, unsigned long *addr)
{
  struct user_regs_struct regs;
  long data;

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "Failed to get child ip\n");
    return false;
  }

  errno = 0;
  data = ptrace(PTRACE_PEEKTEXT, trace_info->child_pid, (void*)regs.rsp, 0);

  if(errno) {
    return false;
  }

  *addr = data;

  return true;
}

static bool
trace_single_step(trace_info_t *trace_info)
{
  int status;

  if(ptrace(PTRACE_SINGLESTEP, trace_info->child_pid, NULL, NULL) < 0) {
    fprintf(stderr, "ptrace(PTRACE_SINGLESTEP) failed\n");
    return false;
  }

  waitpid(trace_info->child_pid, &status, 0);

  if(WIFEXITED(status) || WIFSIGNALED(status)) {
    trace_info->child_pid = -1;
    return false;
  }

  return true;
}

int
trace_count_instructions(trace_info_t *trace_info,
                         function_information_t *func_info,
                         size_t *count)
{
  unsigned long return_address;
  unsigned long ip;
  int status;
  struct user_regs_struct regs;

  *count = 0;

  trace_get_return_address(trace_info, &return_address);

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "failed to get child register values\n");
    return false;
  }

  regs.rip = regs.rip - 1;

  if(ptrace(PTRACE_SETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    return false;
  }

  if(!trace_restore_function(trace_info, func_info)) {
    fprintf(stderr, "Failed to restore function\n");
    return false;
  }

  if(!trace_single_step(trace_info)) {
    return false;
  }

  if(!trace_add_breakpoint(trace_info->child_pid, func_info)) {
    fprintf(stderr, "trace_add_breakpoint failed\n");
    return false;
  }

  (*count)++;

  while(true) {
    trace_get_child_ip(trace_info, &ip);

    if(ip == return_address) {
      break;
    }

    if(!trace_single_step(trace_info)) {
      return false;
    }
    (*count)++;
  }

  if(!trace_restore_function(trace_info, func_info)) {
    fprintf(stderr, "Failed to restore function\n");
    return false;
  }

  if(ptrace(PTRACE_SINGLESTEP, trace_info->child_pid, NULL, NULL) < 0) {
    fprintf(stderr, "ptrace(PTRACE_SINGLESTEP) failed\n");
    return false;
  }

  waitpid(trace_info->child_pid, &status, 0);

  if(WIFEXITED(status) || WIFSIGNALED(status)) {
    trace_info->child_pid = -1;
    return false;
  }

  if(!trace_add_breakpoint(trace_info->child_pid, func_info)) {
    fprintf(stderr, "trace_add_breakpoint failed\n");
    return false;
  }

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "failed to get child register values\n");
    return false;
  }

  if(ptrace(PTRACE_CONT, trace_info->child_pid, NULL, NULL) < 0) {
    fprintf(stderr, "ptrace(PTRACE_CONT) failed\n");
    return false;
  }

  return true;
}


bool
trace_continue(trace_info_t *trace_info, function_information_t *func_info)
{
  int status;
  struct user_regs_struct regs;

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "failed to get child register values\n");
    return false;
  }

  regs.rip = regs.rip - 1;

  if(ptrace(PTRACE_SETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    return false;
  }

  if(!trace_restore_function(trace_info, func_info)) {
    fprintf(stderr, "Failed to restore function\n");
    return false;
  }

  if(ptrace(PTRACE_SINGLESTEP, trace_info->child_pid, NULL, NULL) < 0) {
    fprintf(stderr, "ptrace(PTRACE_SINGLESTEP) failed\n");
    return false;
  }

  waitpid(trace_info->child_pid, &status, 0);

  if(WIFEXITED(status) || WIFSIGNALED(status)) {
    trace_info->child_pid = -1;
    return false;
  }

  if(!trace_add_breakpoint(trace_info->child_pid, func_info)) {
    fprintf(stderr, "trace_add_breakpoint failed\n");
    return false;
  }

  if(ptrace(PTRACE_GETREGS, trace_info->child_pid, NULL, &regs) < 0) {
    fprintf(stderr, "failed to get child register values\n");
    return false;
  }

  if(ptrace(PTRACE_CONT, trace_info->child_pid, NULL, NULL) < 0) {
    fprintf(stderr, "ptrace(PTRACE_CONT) failed\n");
    return false;
  }

  return true;
}

static function_information_t *
trace_find_function_info(trace_info_t *trace_info, unsigned long ip) {

  int i;

  for(i = 0; i < trace_info->num_func_infos; i++) {
    if(trace_info->function_infos[i].ip == (ip - 1)) {
      return  &trace_info->function_infos[i];
    }
  }

  return NULL;
}

static bool
trace_function_call(trace_info_t *trace_info, results_t *results)
{
  unsigned long ip;
  function_information_t *func_info;
  size_t count;
  bool res;

  (void) results;

  if(!trace_get_child_ip(trace_info, &ip)) {
    return false;
  }

  func_info = trace_find_function_info(trace_info, ip);

  if(func_info == NULL) {
    if(ptrace(PTRACE_CONT, trace_info->child_pid, NULL, NULL) < 0) {
      fprintf(stderr, "ptrace(PTRACE_CONT) failed\n");
      return false;
    }

    return true;
  }

  res = trace_count_instructions(trace_info, func_info, &count);

  if(res) {
    return true;
  }
  else {
    return false;
  }
}

bool
trace_function_calls(trace_info_t *trace_info, results_t *results)
{
  int res;
  int status;

  while(true) {

    res = waitpid(trace_info->child_pid, &status, 0);

    if(res == -1) {
      fprintf(stderr, "waitpid returned -1, error is %d\n", errno);
      return false;
    }

    if(WIFEXITED(status)) {
      printf("Program exited with exit code %d\n", WEXITSTATUS(status));
      break;
    }

    if(WIFSIGNALED(status)) {
      printf("Program terminated by signal %d\n", WTERMSIG(status));
      break;
    }

    if(!trace_function_call(trace_info, results)) {
      return false;
    }
  }

  return true;
}

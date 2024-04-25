#include <stdint.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <iostream>
#include <vector>
#include <deque>
#include <optional>
#include <functional>

#include "Pipe_t.h"

enum class Mode : int
{
    empty      = 0,
    number     = 1,
    operators  = 2,
    identifier = 3,
    seperator  = 4,
    string     = 6,
    done       = 7,
    _Mode_size,
    _check_needed,
    _unreachable
};

enum class eSymbol : int
{
    space=0,
    digit,
    operators,
    seperator, // ()[]{}
    quote,     // ''' or '"'
    others,    // alphabet, '$', '.', or '_'
    _eSymbol_size,
};

enum class eOperator : int
{
    e_and,                // &&
    e_or,                 // ||
    pipe,                 // |
    background,           // &
    pipe_bg,              // |&
    in_redirect,          // <
    out_redirect,         // >
    out_append_redirect,  // >>
    _eOperator_size
};

struct Operator_type_pair
{
    Operator_type_pair(){}

    Operator_type_pair(std::string str, enum eOperator type) : str(std::move(str)), type(type){}

    std::string str;
    enum eOperator type;
};

static const Operator_type_pair gOperator_type_table[int(eOperator::_eOperator_size)] = 
{
    Operator_type_pair("&&", eOperator::e_and),
    Operator_type_pair("||", eOperator::e_or),
    Operator_type_pair("|",  eOperator::pipe),
    Operator_type_pair("&",  eOperator::background),
    Operator_type_pair("<",  eOperator::in_redirect),
    Operator_type_pair(">",  eOperator::out_redirect),
    Operator_type_pair(">>", eOperator::out_append_redirect),
};

const enum Mode gTransistion_table[int(Mode::_Mode_size)][int(eSymbol::_eSymbol_size)] = 
{
    //space,      digit,               operator,         seperator         quote                 others  
    {Mode::empty, Mode::number,        Mode::operators,  Mode::seperator,  Mode::string,         Mode::identifier},     // empty
    {Mode::done,  Mode::number,        Mode::done,       Mode::done,       Mode::done,           Mode::_unreachable},   // number
    {Mode::done,  Mode::done,          Mode::operators,  Mode::done,       Mode::done,           Mode::done},           // operators
    {Mode::done,  Mode::identifier,    Mode::done,       Mode::done,       Mode::done,           Mode::identifier},     // identifier
    {Mode::done,  Mode::_check_needed, Mode::done,       Mode::done,       Mode::done,           Mode::_check_needed},  // seperator
    {Mode::done,  Mode::_check_needed, Mode::done,       Mode::done,       Mode::_check_needed,  Mode::_check_needed},  // string
    {Mode::empty, Mode::number,        Mode::operators,  Mode::seperator,  Mode::string,         Mode::identifier},     // done
};

struct Token;
struct Shell_instruction_list;


static bool Is_operator(int c);
static bool Is_parenthesis(int c);
static bool Is_mid_bracket(int c);
static bool Is_big_bracket(int c);
static bool Is_open_seperator(int c);
static bool Is_close_seperator(int c);
static bool Is_seperator(int c);
static bool Is_others(int c);
static enum eSymbol Check_symbol(int c);
static bool Get_operator_type_pair(const std::string &src, Operator_type_pair *dst);
static bool Parse_cmd(const std::string &str, std::deque<Token> *dst);
static bool Preprocess_step(const std::deque<Token> &token_list, Shell_instruction_list &dst);

static bool Execute_entire_cmd(Shell_instruction_list &src);

static std::function<bool(void)> Get_builtin_func(Shell_instruction_list &src);


struct Token
{
    Token(std::string data, std::size_t start, std::size_t end, enum Mode type) 
          : data(std::move(data)), 
            start(start), 
            end(end), 
            type(type)
    {

    }
    std::string data;
    std::size_t start, end;
    enum Mode type;
};

struct Shell_instruction_list
{
    int in_open_mode = 0, out_open_mode = 0;
    int in_fd = -1, out_fd = -1;
    std::size_t cmd_count = 1;
    std::string in_file_name, out_file_name;
    std::deque<std::string> instruction_list;

    // collect a list of string char* for execv family commands
    std::vector<const char*> cmd_list;
    
    // tranmit data between pipes
    Pipe_t pipe_var[2];

    // record which pipe is used for read.
    // if one is for read, the other pipe is for wirte
    //exp: cur_read_pipe = 0 => pipe_var[0] is for read, pipe[1] is for write
    int cur_read_pipe = 0;

    // the first input source of the first command can be different from commands having other positions. 
    bool is_first_cmd = true;

    bool exit_flag = false;
    
    Shell_instruction_list() 
    {
        old_stdin = dup(STDIN_FILENO);
        old_stdout = dup(STDOUT_FILENO);
    }
    std::size_t size() const {return instruction_list.size();}
    void open_in_file() {open_file(in_file_name, &in_fd, STDIN_FILENO, in_open_mode); }
    void open_out_file() {open_file(out_file_name, &out_fd, STDOUT_FILENO, out_open_mode);}
    ~Shell_instruction_list()
    {
        close(in_fd);
        close(out_fd);
        in_fd = out_fd = -1;

        // restore stdin and stdout
        dup2(STDIN_FILENO, old_stdin);
        old_stdin = -1;

        dup2(STDOUT_FILENO, old_stdout);
        old_stdout = -1; 
 


    }
    
private:
    
    void open_file(const std::string &name, int *fd, int redirected_fd, int open_mdoe)
    {
        if (name != "")
        {
            *fd = open(name.c_str(), open_mdoe);
            if (*fd == -1)
            {
                std::cout << "fail to open the file " << name << "\n";
                abort();
            }
            dup2(*fd, redirected_fd);
            close(*fd);
        }
    }
    int old_stdin, old_stdout ;
};


int main()
{

    for(bool is_success = true, live = true ; is_success == true && live == true ;)
    {
        std::cout << "simple shell> ";
        std::string str;
        std::cin.clear();

        std::getline(std::cin, str);

        std::deque<Token> token_list ;
        is_success = Parse_cmd(str, &token_list);
       
        if (is_success != true)
            break;
        

        Shell_instruction_list shell_instruction_list;
        is_success = Preprocess_step(token_list, shell_instruction_list);
        if (is_success != true)
        {
            printf("syntax error\n");
            break;
        }

        live = Execute_entire_cmd(shell_instruction_list);    
    }


}


static bool Is_operator(int c)
{
    switch(c)
    {
        case '+': return true;
        case '-': return true;
        case '*': return true;
        case '/': return true;
        case '%': return true;
        case '.': return true;
        case ':': return true;
        case '?': return true;
        case '&': return true;
        case '|': return true;
        case '^': return true;
        case '~': return true;
        case '>': return true;
        case '<': return true;                
        default : return false;
    }

    return false;
}

static bool Is_parenthesis(int c)
{
    if (c == '(' || c == ')')
        return true;

    return false;
}

static bool Is_mid_bracket(int c)
{
    if (c == '[' || c == ']')
        return true;

    return false;
}

static bool Is_big_bracket(int c)

{
    if (c == '{' || c == '}')
        return true;

    return false;
}

static bool Is_open_seperator(int c)
{
    return c == '(' || c == '[' || c == '{';
}

static bool Is_close_seperator(int c)
{
    return c == ')' || c == ']' || c == '}';
}

static bool Is_seperator(int c)
{
    return Is_open_seperator(c) || Is_close_seperator(c) || c == '\0';
}

static bool Is_others(int c)
{
    return isalpha(c) || c == '$' || c == '_' || c == '.' || c == '/';
}

static enum eSymbol Check_symbol(int c)
{
    enum eSymbol symbol;
    
    if (std::isdigit(c))
        symbol = eSymbol::digit;
    else if (Is_others(c))
        symbol = eSymbol::others;   
    else if (Is_operator(c))
        symbol = eSymbol::operators;   
    else if (Is_seperator(c))
        symbol = eSymbol::seperator;
    else if (c == ' ') // space
        symbol = eSymbol::space;
    else
    {
        std::cout << c << " ??" << "\n";
        abort();
    }
    return symbol;
}

static bool Get_operator_type_pair(const std::string &src, Operator_type_pair *dst)
{
    for(auto &item : gOperator_type_table)
    {
        if (item.str == src)
        {
            *dst = item;
            return true;
        }
    }
    return false;
}


static bool Parse_cmd(const std::string &str, std::deque<Token> *dst)
{   
    std::deque<Token> token_list {};

    enum Mode mode = Mode::empty;

    auto tail_it = str.begin();
    auto head_it = tail_it;
    
    for( ;  ; head_it++)
    {
        enum eSymbol symbol = Check_symbol(*head_it);
        
        auto old_mode = mode;

        mode = gTransistion_table[int(mode)][int(symbol)];

        // TODO : complete other '_check_needed' check
        if (mode == Mode::_check_needed)
        {
            if (old_mode == Mode::seperator)
            {
                if (Is_open_seperator(*(head_it-1)))
                    mode = Mode::done;
                else
                    mode = Mode::_unreachable;
            }
        }

        if (mode == Mode::_unreachable)
        {
            // substring length+1 to capture the character which leads to the syntax error
            std::cout << "syntax error: " << str.substr(tail_it - str.begin(), head_it - tail_it + 1) << "\n";
            std::cout << "string [" << tail_it - str.begin() << ":" << head_it - tail_it + 1 << "]\n";
            return false;
        }
        if (mode == Mode::done)
        {
            auto str_token = str.substr(tail_it - str.begin(), head_it - tail_it);
            
            token_list.emplace_back(std::move(str_token), tail_it - str.begin(), head_it - str.begin() - 1, old_mode);

            tail_it = head_it;
            
            while(*tail_it == ' ') // eliminate space symbols, then the next token will not start with space symbols
                tail_it++;
            head_it = tail_it;
            
            mode = gTransistion_table[int(Mode::done)][int(symbol)];

            if (head_it == str.end())
                break;

            head_it -= 1; // minus 1, then resume from the cut point
        }
    }

/*     for(auto &item : std::as_const(token_list))
        std::cout << item.data << ":[" << item.start << ":" << item.end << "]  type:" << int(item.type) << "\n"; */
    
    *dst = token_list;
    return true;
}

static bool Preprocess_step(const std::deque<Token> &token_list, Shell_instruction_list &dst)
{
    for(std::size_t i = 0 ; i < token_list.size() ; i++)
    {
        if (token_list[i].type == Mode::identifier || token_list[i].type == Mode::number)
            dst.instruction_list.push_back(token_list[i].data);
        else if (token_list[i].data == "-")
        {
            if (i+1 >= token_list.size())
                return false;

            if (token_list[i+1].type != Mode::identifier)
                return false;  

            auto tmp = token_list[i].data + token_list[i+1].data ;
            dst.instruction_list.push_back(std::move(tmp));
            i++;
        }
        else if (token_list[i].data == "<")
        {
            if (i+1 >= token_list.size())
                return false;
            if (token_list[i+1].type != Mode::identifier)
                return false;  

            dst.instruction_list.push_back(token_list[i].data);
            dst.in_file_name = token_list[i+1].data;
            dst.in_open_mode = O_RDONLY;
        }
        else if (token_list[i].data == ">")
        {
            if (i+1 >= token_list.size())
                return false;
            if (token_list[i+1].type != Mode::identifier)
                return false;  

            dst.instruction_list.push_back(token_list[i].data);
            dst.out_file_name = token_list[i+1].data;
            dst.out_open_mode = O_WRONLY|O_CREAT|O_TRUNC;
        }
        else if (token_list[i].data == ">>")
        {
            if (i+1 >= token_list.size())
                return false;
            if (token_list[i+1].type != Mode::identifier)
                return false;  

            dst.instruction_list.push_back(token_list[i].data);
            dst.out_file_name = token_list[i+1].data;
            dst.out_open_mode = O_WRONLY|O_APPEND|O_CREAT|O_APPEND;
        }
        else
        {
            if (token_list[i].data == "|")
                dst.cmd_count += 1;
            dst.instruction_list.push_back(token_list[i].data);
        }
    }    
    return true;
}

// if it's not a builtin function, an empty std::function is returned
static std::function<bool(void)> Get_builtin_func(Shell_instruction_list &src)
{
    auto invalid_butilin = []()
    {
        return false;
    };

    // 'nullptr' at the end of src.cmd_list contributes extra size of 1 
    if (strcmp(src.cmd_list[0], "cd") == 0)
    {
        if (src.cmd_list.size() == 2 + 1)
        {
            return [&src]()
            {
                return chdir(src.cmd_list[1]) == 0;
            };
        }
        else
            return invalid_butilin;
    }
    else if (strcmp(src.cmd_list[0], "exit") == 0)
    {
        if (src.cmd_list.size() == 1 + 1)
        {
            return [&src]()
            {
                src.exit_flag = true;
                return true;
            };
        }
        else
            return invalid_butilin;
    }
    else if (strcmp(src.cmd_list[0], "whoisauthor") == 0)
    {
        if (src.cmd_list.size() == 1 + 1)
        {
            return []()
            {
                std::cout << "QQmental\n";
                return true;
            };
        }
        else
            return invalid_butilin;
    }

    //not a builtin function
    return {};
}

// return false if the shell needed to be terminated
// src could be modified for executing shell commands
static bool Execute_entire_cmd(Shell_instruction_list &src)
{
    // cmd is executed when it meets operators like pipe, output redirect or it is the last command
    auto execute_cmd = [](Shell_instruction_list &src, Operator_type_pair &type_pair) -> bool
    {
        if (fork() == 0)
        {   
            // I have no idea that why input file redirection in parent process can lead to bugs, 
            // so open_in_file() is invoked in child process
            if (src.is_first_cmd == true)
                src.open_in_file();
            else
            {
                src.pipe_var[src.cur_read_pipe].Close_write_end();
                dup2(src.pipe_var[src.cur_read_pipe].read_end(), STDIN_FILENO);
            }
                
            // cmd_count == 1 should be detected before detecting operator type, because the last cmd has no defined operator type
            // TODO: there is an exception, backgroud operator &
            // & is an operator put in the back of a command
            // this is not implemented in this simple shell
            // If you implement this, the following program is needed to be modified. 
            if (src.cmd_count == 1)
                src.open_out_file(); 
            else if (type_pair.type == eOperator::pipe)
                dup2(src.pipe_var[!src.cur_read_pipe].write_end(), STDOUT_FILENO);
            
            execvp(src.cmd_list.data()[0], const_cast<char**>(src.cmd_list.data()));
            fprintf(stderr, "Cannot run %s\n", src.cmd_list.data()[0]);
            return false;
        }
        else
        {
            src.pipe_var[src.cur_read_pipe].Close_write_end();
            wait(nullptr);

            // It is in vain to modify 'is_first_cmd' in child process, because it's just a copy
            if (src.is_first_cmd == true)
                src.is_first_cmd = false;
        }
        return true;
    };
    
    for(std::size_t i = 0 ; i < src.size() && src.cmd_count != 0 ; i++)
    {
        Operator_type_pair type_pair;
        
        auto is_operator = Get_operator_type_pair(src.instruction_list[i].data(), &type_pair);
        
        if (is_operator == false)
            src.cmd_list.push_back(src.instruction_list[i].data());
        
        //if it meets an operator or the last token, a command is executed
        if (is_operator == true || i + 1 == src.size())
        {
            src.cmd_list.push_back(nullptr);  // __argv in execv* should ends with nullptr
            
            std::function<bool(void)> builtin_func = Get_builtin_func(src);

            bool is_success = true;

            if (builtin_func)
                is_success = builtin_func();                        // execute builtin function
            else  
                is_success = execute_cmd(src, type_pair);           // execute non-builtin function

            if (src.exit_flag == true)                              // command 'exit' is called
                return false;
  
            src.pipe_var[src.cur_read_pipe].Close();
            pipe(src.pipe_var[src.cur_read_pipe].fd_s());           // reinitialize pipe     
            src.cur_read_pipe = !src.cur_read_pipe;                 // switch read pipe         
            src.cmd_count -= 1;                                     // the rest cmd -1
            src.cmd_list.clear();
            if (   type_pair.type == eOperator::out_append_redirect 
                || type_pair.type == eOperator::out_redirect
                || type_pair.type == eOperator::in_redirect)
                i++;
        }
    }
    return true;
}

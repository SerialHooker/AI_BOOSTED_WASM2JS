#include <chrono>
#include <iostream>
#include "Utils.h"
#include "Parser.h"
using namespace std;
// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or
// click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.
int main() {
    Parser parser = Parser();
    std::string path, name;
    std::tie(path, name) = Utils::AskForPathAndName("wasm");
    bool AI;
    std::cout << "Do you want to use AI to summarize and rename the code? (y/n): ";
    char choice;
    std::cin >> choice;
    AI = (choice == 'y' || choice == 'Y');
    std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();
    std::string result = parser.Parse(path.c_str(), AI, ModuleFormat::IIFE);
    std::chrono::system_clock::time_point endTime = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed = std::chrono::system_clock::now() - startTime;
    std::cout << "result : " << result;
    std::cout << " elapsed : " << elapsed.count() << " seconds" << std::endl;
    Utils::writeToFile(name, result);
    return 0;
}

// TIP See CLion help at <a
// href="https://www.jetbrains.com/help/clion/">jetbrains.com/help/clion/</a>.
//  Also, you can try interactive lessons for CLion by selecting
//  'Help | Learn IDE Features' from the main menu.
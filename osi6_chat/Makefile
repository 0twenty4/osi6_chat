CXX := g++

CXXFLAGS := -std=c++2a -Wall -pthread 
LDFLAGS := -lncursesw -lboost_locale

# Исходники
SRCS := main.cpp

# Имя исполняемого файла
TARGET := server

# Сборка
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

# Очистка
clean:
	rm -f $(TARGET)

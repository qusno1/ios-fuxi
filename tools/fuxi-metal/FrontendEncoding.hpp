/* Command-local encoding shortcuts; ownership stays in the command's resource set. */
#pragma once
#include <array>
#include <functional>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include "ShadowRanges.hpp"
namespace fuxi {
class CommandResourceCache {
 struct Entry {const void *resource=nullptr;uint32_t reasons=0;};
 std::array<Entry,64> entries{};
public:
 bool seen(const void *resource,bool bounded,ShadowFullReason reason=ShadowFullReason::other){
  if(!resource)return true;
  auto &e=entries[(uintptr_t(resource)>>4)%entries.size()];uint32_t bit=bounded?1u:2u<<size_t(reason);
  if(e.resource==resource){if(bounded || (e.reasons&bit))return true;e.reasons|=bit;}
  else e={resource,bit};return false;
 }
 void clear(){entries={};}
};
// Text built on first use (failures only). Copies share one state; the producer runs at most once and is released afterwards.
class LazyText {
 struct State {std::function<std::string()> make;std::once_flag once;std::string value;};
 std::shared_ptr<State> state;
public:
 LazyText()=default;
 explicit LazyText(std::function<std::string()> make):state(std::make_shared<State>()){state->make=std::move(make);}
 const char *c_str() const{
  if(!state)return "";
  std::call_once(state->once,[&]{try{state->value=state->make();}catch(...){}state->make=nullptr;});
  return state->value.c_str();
 }
 bool empty() const{return !state;}
 bool same(const LazyText &other) const{return state==other.state;}
};
// Draws share immutable diagnostic text; failures still report the full snapshot.
class OperationDiagnostic {
 std::shared_ptr<const std::string> text;LazyText lazy;
public:
 OperationDiagnostic()=default;
 OperationDiagnostic &operator=(const char *value){text=value && *value?std::make_shared<const std::string>(value):nullptr;lazy=LazyText();return *this;}
 OperationDiagnostic &operator=(const LazyText &value){lazy=value;text=nullptr;return *this;}
 bool empty() const{return !text && lazy.empty();}
 void clear(){text.reset();lazy=LazyText();}
 const char *c_str() const{return text?text->c_str():lazy.c_str();}
 friend bool operator==(const OperationDiagnostic &a,const OperationDiagnostic &b){
  if(a.lazy.same(b.lazy) && a.text==b.text)return true;
  return !strcmp(a.c_str(),b.c_str()) && a.empty()==b.empty();
 }
 friend std::string operator+(std::string a,const OperationDiagnostic &b){return a+=b.c_str();}
};
}
